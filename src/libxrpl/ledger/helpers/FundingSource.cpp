#include <xrpl/ledger/helpers/FundingSource.h>

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/beast/utility/Zero.h>
#include <xrpl/beast/utility/instrumentation.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/ledger/helpers/AccountRootHelpers.h>
#include <xrpl/ledger/helpers/SponsorHelpers.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XRPAmount.h>

#include <algorithm>
#include <cstdint>

namespace xrpl {

namespace {

// Stamp the reserve sponsor's AccountID onto a newly created sponsored
// ledger entry. Called from adjustOwnerCount when delta > 0 on a reserve-
// sponsored tx — never invoked directly by transactors.
void
stampReserveSponsor(SLE::ref objectSle, TxPayers const& payers, SF_ACCOUNT const& sponsorField)
{
    XRPL_ASSERT(
        payers.reserveSource.accountId != payers.initiator,
        "stampReserveSponsor : reserve must be sponsored");
    XRPL_ASSERT(
        (objectSle->getType() == ltRIPPLE_STATE &&
         (sponsorField == sfHighSponsor || sponsorField == sfLowSponsor)) ||
            (objectSle->getType() != ltRIPPLE_STATE && sponsorField == sfSponsor),
        "stampReserveSponsor : valid sponsor field for ledger entry type");
    objectSle->setAccountID(sponsorField, payers.reserveSource.accountId);
}

}  // namespace

TxPayers
resolveTxPayers(ReadView const& view, STTx const& tx)
{
    auto const initiator = tx.getAccountID(sfAccount);
    bool const hasSponsor = tx.isFieldPresent(sfSponsor);
    bool const isCoSigned = tx.isFieldPresent(sfSponsorSignature);
    bool const isDelegate = tx.isFieldPresent(sfDelegate);

    auto const sponsorshipKeylet = hasSponsor
        ? keylet::sponsorship(tx.getAccountID(sfSponsor), initiator)
        : Keylet{ltSPONSORSHIP, uint256{}};

    auto const buildFeeSource = [&]() -> FundingSource {
        bool const feeSponsored = hasSponsor && isFeeSponsored(tx);
        if (!feeSponsored)
        {
            // tx.getFeePayer() returns sfDelegate if present, else sfAccount.
            auto const payerId = tx.getFeePayer();
            return FundingSource{
                .kind = FundingKind::AccountRoot,
                .keylet = keylet::account(payerId),
                .accountId = payerId,
            };
        }

        auto const sponsorId = tx.getAccountID(sfSponsor);
        // Preserves existing dispatch: if the Sponsorship SLE exists the
        // fee comes from it; otherwise a sponsor signature pays the fee
        // from the sponsor's account_root.
        if (isCoSigned && !view.exists(sponsorshipKeylet))
            return FundingSource{
                .kind = FundingKind::AccountRoot,
                .keylet = keylet::account(sponsorId),
                .accountId = sponsorId,
            };
        return FundingSource{
            .kind = FundingKind::Sponsorship,
            .keylet = sponsorshipKeylet,
            .accountId = sponsorId,
        };
    };

    auto const buildReserveSource = [&]() -> FundingSource {
        bool const reserveSponsored = hasSponsor && isReserveSponsored(tx);
        if (!reserveSponsored)
            return FundingSource{
                .kind = FundingKind::AccountRoot,
                .keylet = keylet::account(initiator),
                .accountId = initiator,
            };

        auto const sponsorId = tx.getAccountID(sfSponsor);
        return FundingSource{
            .kind = FundingKind::AccountRoot,
            .keylet = keylet::account(sponsorId),
            .accountId = sponsorId,
        };
    };

    return TxPayers{
        .initiator = initiator,
        .feeSource = buildFeeSource(),
        .reserveSource = buildReserveSource(),
        .isCoSigned = isCoSigned,
        .isDelegate = isDelegate,
    };
}

XRPAmount
sourceBalance(SLE::const_ref sle, FundingKind kind)
{
    if (!sle)
        return beast::kZero;
    auto const& field = balanceField(kind);
    if (!sle->isFieldPresent(field))
        return beast::kZero;
    return sle->getFieldAmount(field).xrp();
}

XRPAmount
capFeeBySource(SLE::const_ref sle, FundingKind kind, XRPAmount amount)
{
    if (kind != FundingKind::Sponsorship || !sle || !sle->isFieldPresent(sfMaxFee))
        return amount;
    return std::min(amount, sle->getFieldAmount(sfMaxFee).xrp());
}

TER
debitSource(ApplyView& view, FundingSource const& src, XRPAmount amount)
{
    auto sle = view.peek(src.keylet);
    if (!sle)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const& field = balanceField(src.kind);
    auto const after = sle->getFieldAmount(field) - amount;
    if (after == beast::kZero && src.kind == FundingKind::Sponsorship)
    {
        XRPL_ASSERT(field == sfFeeAmount, "debitSource: Sponsorship fee field is not sfFeeAmount");
        sle->makeFieldAbsent(field);
    }
    else
        sle->setFieldAmount(field, after);

    view.update(sle);
    return tesSUCCESS;
}

TER
checkReserve(
    ApplyView& view,
    TxPayers const& payers,
    std::int32_t ownerCountDelta,
    std::int32_t reserveCountDelta,
    beast::Journal j)
{
    bool const reserveSponsored = payers.reserveSource.accountId != payers.initiator;
    if (reserveSponsored && ownerCountDelta > 0)
    {
        auto const sponsorshipSle =
            view.peek(keylet::sponsorship(payers.reserveSource.accountId, payers.initiator));
        if (sponsorshipSle)
        {
            auto const remaining = sponsorshipSle->getFieldU32(sfRemainingOwnerCount);
            if (remaining < static_cast<std::uint32_t>(ownerCountDelta))
                return tecINSUFFICIENT_RESERVE;
        }
    }

    auto const reserveSle = view.read(payers.reserveSource.keylet);
    if (!reserveSle)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    auto const reserve = accountReserve(view, reserveSle, j, ownerCountDelta, reserveCountDelta);
    auto const balance = reserveSle->getFieldAmount(sfBalance);
    if (balance < reserve)
        return tecINSUFFICIENT_RESERVE;
    return tesSUCCESS;
}

void
adjustOwnerCount(
    ApplyView& view,
    SLE::ref objectSle,
    TxPayers const& payers,
    std::int32_t delta,
    beast::Journal j,
    SF_ACCOUNT const& sponsorField)
{
    bool const reserveSponsored = payers.reserveSource.accountId != payers.initiator;

    if (delta > 0 && reserveSponsored)
        stampReserveSponsor(objectSle, payers, sponsorField);

    auto const accountSle = view.peek(keylet::account(payers.initiator));
    SLE::pointer const sponsorSle =
        reserveSponsored ? view.peek(payers.reserveSource.keylet) : SLE::pointer{};
    adjustOwnerCount(view, accountSle, sponsorSle, delta, j);
}

}  // namespace xrpl
