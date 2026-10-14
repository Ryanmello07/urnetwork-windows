// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The v1 claim dialog's gas (App/EarningsSheets.cpp, which needs WinUI, so it
// is read from the source). On runtime 478 the SDK signs every v1 claim with
// its estimate plus a quarter and at least 2,700,000 gas, below which the vault
// accepts a claim but defers its payment, and sends it only when the gas key
// holds that limit at 1.1 x the gas price, else the claim fails needs_gas
// (VERSION2-LAUNCH.md §2.6).

// VERSION2-LAUNCH.md §2.6: the funding hint asks for 0.02 TAO, a v2 claim's,
// which covers the SDK's v1 floor up to about 6.7 gwei
func TestV1ClaimGasTopUpCoversTheSdkFloor(t *testing.T) {
	constant := func(file string, declaration string) float64 {
		t.Helper()
		source := stripComments(readAppSource(t, file))
		match := regexp.MustCompile(`(?m)^` + regexp.QuoteMeta(declaration) + ` = ([0-9.]+);$`).FindStringSubmatch(source)
		if match == nil {
			t.Fatalf("%s: no %s", file, declaration)
		}
		value, err := strconv.ParseFloat(match[1], 64)
		if err != nil {
			t.Fatal(err)
		}
		return value
	}
	// the SDK's least v1 claim gas limit (sdk/sn_claims.go snClaimGasFloor) at
	// its legacy price, 1.1 x a 5 gwei gas price, in TAO: 0.01485
	const floorTao = 2_700_000 * 1.1 * 5e9 / 1e18
	suggested := constant("EarningsSheets.cpp", "constexpr double kSuggestedGasTao")
	ledger := constant("SnPayoutPresentation.h", "inline constexpr double kLedgerClaimGasTao")
	if suggested != 0.02 || suggested != ledger {
		t.Errorf("kSuggestedGasTao = %v, want 0.02, a v2 claim's %v", suggested, ledger)
	}
	if suggested < floorTao {
		t.Errorf("kSuggestedGasTao = %v is below the SDK's v1 floor, %v TAO at 5 gwei", suggested, floorTao)
	}
	build := functionBody(stripComments(readAppSource(t, "EarningsSheets.cpp")), "void ClaimAlphaSheet::Build(")
	if !strings.Contains(build, `urnw::Format("send_tao_to_mirror", FormatTao(kSuggestedGasTao),`) {
		t.Error("ClaimAlphaSheet::Build: the funding hint does not ask for kSuggestedGasTao")
	}
}
