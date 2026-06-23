#pragma once

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/ledger/ApplyView.h>
#include <xrpl/ledger/ReadView.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Keylet.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STLedgerEntry.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/XRPAmount.h>

#include <cstdint>

namespace xrpl {

// FundingSource hides the AccountRoot-vs-Sponsorship-object dispatch behind
// one type, so call sites that need to read or debit a balance don't have to
// branch on "is this sponsored, and if so, how" at every step.
enum class FundingKind {
    AccountRoot,  // funded from an Account's sfBalance
    Sponsorship,  // funded from a Sponsorship object's sfFeeAmount
};

struct FundingSource
{
    FundingKind kind;
    Keylet keylet;
    AccountID accountId;
};

// Both funding sources for a transaction, plus tx-level flags that apply
// equally to fee and reserve sources.
struct TxPayers
{
    AccountID initiator;  // tx[sfAccount]
    FundingSource feeSource;
    FundingSource reserveSource;
    bool isCoSigned;  // tx has sfSponsorSignature
    bool isDelegate;  // tx has sfDelegate
};

// The SField used to read or write the balance on this source's SLE.
inline SF_AMOUNT const&
balanceField(FundingKind kind)
{
    return kind == FundingKind::Sponsorship ? sfFeeAmount : sfBalance;
}

// Resolve fee + reserve funding sources for `tx` against `view`.
// Pure function of inputs; safe to call from preclaim or doApply.
[[nodiscard]] TxPayers
resolveTxPayers(ReadView const& view, STTx const& tx);

// Read the spendable balance from a source's SLE. Returns beast::kZero when
// the SLE is null or the balance field is absent.
[[nodiscard]] XRPAmount
sourceBalance(SLE::const_ref sle, FundingKind kind);

// Apply a Sponsorship object's sfMaxFee cap, if present. For AccountRoot
// sources this is a no-op and returns `amount` unchanged.
[[nodiscard]] XRPAmount
capFeeBySource(SLE::const_ref sle, FundingKind kind, XRPAmount amount);

// Deduct `amount` from `src`'s balance field. Handles the Sponsorship case
// where draining sfFeeAmount to zero requires makeFieldAbsent (sfFeeAmount
// is soeOptional on ltSPONSORSHIP). Updates the SLE in `view`.
[[nodiscard]] TER
debitSource(ApplyView& view, FundingSource const& src, XRPAmount amount);

// Verify the reserve payer has sufficient balance for the proposed owner-
// count and reserve-count deltas. When a Sponsorship SLE exists, also caps
// against its sfRemainingOwnerCount.
[[nodiscard]] TER
checkReserve(
    ApplyView& view,
    TxPayers const& payers,
    std::int32_t ownerCountDelta,
    std::int32_t reserveCountDelta,
    beast::Journal j);

// Apply +/- delta to every owner-count field implicated by this tx, and (for
// positive deltas on sponsored txs) stamp the reserve sponsor onto the new
// ledger entry's sponsor field. Counts updated:
//   initiator.OwnerCount               (always)
//   initiator.SponsoredOwnerCount      (if reserve-sponsored)
//   reserveSponsor.SponsoringOwnerCount(if reserve-sponsored)
//   Sponsorship.RemainingOwnerCount    (-delta, when SLE exists and delta > 0)
void
adjustOwnerCount(
    ApplyView& view,
    SLE::ref objectSle,
    TxPayers const& payers,
    std::int32_t delta,
    beast::Journal j,
    SF_ACCOUNT const& sponsorField = sfSponsor);

}  // namespace xrpl
