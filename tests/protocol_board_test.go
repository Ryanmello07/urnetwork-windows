// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"html"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Protocol leaderboard and the Earnings Top 200 tile (owner decision
// 2026-10-29, LEADERTOP; apple/LEADERTOP.md is the design): the C++ spec of
// their presentation (App/ProtocolBoardPresentation.h), alone and through the
// generated sdk header's own result types and Api calls; negative controls on
// it; and, for what needs Windows to build, the wiring read from the sources:
// the board's two requests and the fences on their answers, the three boards
// behind one switch with Protocol first and the default, the fold that moves
// the card ahead of the list, the tile's reading of the card's answer, the
// shared name switch, the detail sheet and what a screen reader hears.

// Compile the Protocol board spec (app/tools/protocol-board-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildProtocolBoardTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("protocol board tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "protocol-board-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "protocol-board-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build protocol board tests: %v\n%s", err, output)
	}
	return program
}

func runProtocolBoardTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("protocol board: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the spec: the readers on both shapes a nullable number
// can take, the words, the banners, the chart, the claim prompt, the card in
// every state, the list with the line and the caller's row, the "you" bar,
// the detail sheet, the tile, the list's geometry and the model's fences.
func TestProtocolBoardPresentation(t *testing.T) {
	runProtocolBoardTests(t, buildProtocolBoardTests(t))
}

// The same spec with the readers run on the generated header's result types,
// and the board's two calls made through the generated Api over a fake of the
// C ABI functions it calls: the JSON the wrapper sends (the page size and the
// cursor) and the JSON the sdk answers with, as Go writes it, read the way the
// board reads it. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and
// so does a header from before SnTop.
func TestProtocolBoardPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := protocolBoardSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with SnTop and SnTopMe (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_PROTOCOL_BOARD_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runProtocolBoardTests(t, buildProtocolBoardTests(t, extra...))

	// the sdk carries a null cutoff as 0 with has_claimed_cutoff false: a
	// reader that ignores the flag shows 0
	requireProtocolBoardFailureWith(t, extra, func(source string) string {
		return strings.Replace(source,
			"      return (s).has_##field ? Optional<V>((s).field) : std::nullopt;                   \\\n",
			"      return Optional<V>((s).field);                                                   \\\n", 1)
	}, "sdk header: a null cutoff and precision stay unknown, not 0")
}

// The directory of a urnetwork_sdk.hpp that has SnTop and SnTopMe, or "".
func protocolBoardSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		missing := ""
		for _, want := range []string{"struct SnTopResult {", "struct SnTopMeResult {",
			"void snTop(const std::optional<SnTopArgs>& args, SnTopCallback callback) const;",
			"void snTopMe(SnTopMeCallback callback) const;"} {
			if !strings.Contains(string(header), want) {
				missing = want
				break
			}
		}
		if missing == "" {
			return dir
		}
		if dir == explicit {
			t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no %s", explicit, missing)
		}
		t.Logf("%s: urnetwork_sdk.hpp predates SnTop", dir)
	}
	return ""
}

// Run the spec against a rewritten copy of ProtocolBoardPresentation.h and
// require that it fails, naming want.
func requireProtocolBoardFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	requireProtocolBoardFailureWith(t, nil, mutate, want)
}

// requireProtocolBoardFailure, with the spec built with extra arguments ahead
// of the rewritten header's directory (the sdk header build's).
func requireProtocolBoardFailureWith(t *testing.T, extra []string, mutate func(string) string, want string) {
	t.Helper()
	const header = "ProtocolBoardPresentation.h"
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", header))
	if err != nil {
		t.Fatal(err)
	}
	changed := mutate(string(source))
	if changed == string(source) {
		t.Fatalf("negative control did not change the production %s", header)
	}
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, header), []byte(changed), 0600); err != nil {
		t.Fatal(err)
	}
	// -iquote: searched for the spec's quoted include ahead of the app's -I
	program := buildProtocolBoardTests(t, append(append([]string{}, extra...), "-iquote", dir)...)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control on %s was not detected (want %q): %v\n%s", header, want, err, output)
	}
}

// The spec catches each way the board could misread the server or mislead a
// provider: a nullable number read as 0, rows past 400 listed, a claim prompt
// the server did not flag or one the app reckons for itself (CONTRACT.md,
// amended 2026-10-29: apps use claim_prompt), a claim button while binding is
// closed, the closeness line keyed on the label while
// labels are estimates, a prompt without the warning, the closed banner left
// up once binding opens, a stale banner with no time, the line drawn in the
// wrong place, an anonymous claimed row shown as Anonymous, a chip on every
// row, a chart whose axis is not inverted, the tile's claim and sign while
// binding is closed, the tile's demotion warning gone, a stale answer
// repainting the list, an answer from before a reset applying, a failed
// refresh dropping the rows, two snapshots mixed in one list, a next page
// whose snapshot expired (410) retried with its dead cursor instead of a
// reload from the top, another refusal taken for one, the indicator
// spanning positions an ended list never reaches, the line's height left out
// of the scroll position, a stale incentive shown as fresh, and an
// unmeasurable network's card keeping a prompt.
func TestProtocolBoardPresentationRejectsBrokenReadings(t *testing.T) {
	controls := []struct {
		old, new, want string
	}{
		{"      return (s).has_##field ? Optional<V>((s).field) : std::nullopt;                   \\\n",
			"      return Optional<V>((s).field);                                                   \\\n",
			"has_ flags: a null cutoff and precision stay unknown, not 0"},
		{"  std::erase_if(rows, [](const Row& row) { return row.position <= 0 || kListDepth < row.position; });",
			"  std::erase_if(rows, [](const Row& row) { return row.position <= 0; });",
			"rows out of range and a repeated position are left out"},
		{"  if (!row.claimPrompt) return prompt;", "  if (row.position <= 0) return prompt;",
			"no claim_prompt from the server: no prompt"},
		{"  if (!row.claimPrompt) return prompt;",
			"  if (!row.claimPrompt || !row.measurable || 200 < row.position) return prompt;",
			"the flag decides, the app does not compute it"},
		{"  prompt.button = snapshot.bindingOpen;", "  prompt.button = true;",
			"binding closed: a note, no button"},
		{"    case Label::None: return std::fabs(row.margin) < kNearLine;", "    case Label::None: return false;",
			"within 20% of the line, even while labels are estimates (Q17)"},
		{"  const bool underOne = 0 < size && size * 100 < 0.95;", "  const bool underOne = false;",
			"under 1%, one decimal, never 0%"},
		{"  prompt.warning = text.localized(\"leaderboard_protocol_claim_warning\");\n", "",
			"every prompt warns"},
		{"  if (!snapshot->bindingOpen) banners.push_back(", "  if (true) banners.push_back(",
			"binding open: the banner is removed"},
		{"  if (snapshot->stale && 0 < snapshot->computedAtMillis) {", "  if (snapshot->stale) {",
			"a stale snapshot with no time cannot say when, and says nothing"},
		{"    if (row.position == seats) {", "    if (row.position == seats + 1) {",
			"the line between 200 and 201"},
		{"  if (row.Claimed()) return UidsText(row, text);\n", "",
			"an anonymous claimed row shows only its UIDs (Q12)"},
		{"  if (isMe) {\n    out.you = ", "  if (true) {\n    out.you = ",
			"the chip is the caller's row's alone"},
		{"  auto yOf = [&](int64_t rank) { return static_cast<double>(rank - best) / span; };",
			"  auto yOf = [&](int64_t rank) { return static_cast<double>(worst - rank) / span; };",
			"the best rank on the chart at the top"},
		{"  if (snapshot->bindingOpen) {\n    tile.claimButton", "  if (true) {\n    tile.claimButton",
			"binding closed: no claim, no sign"},
		{"    tile.nearLine = row.margin < kNearLine;", "    tile.nearLine = false;",
			"below the line: the demotion warning"},
		{"    if (request.load != lastLoad_) return Applied::Dropped;", "    if (request.load == 0) return Applied::Dropped;",
			"an older one, answering after it, is dropped"},
		{"    state_ = {};\n    ++lastLoad_;\n    ++lastMe_;\n    open_ = ++lastOpen_;",
			"    state_ = {};\n    ++lastMe_;\n    open_ = ++lastOpen_;",
			"a page from before the reopen is not the latest"},
		{"        // a list on screen stays, with its notice\n        state_.listFailed = true;",
			"        // a list on screen stays, with its notice\n        state_.listFailed = true;\n        state_.listLoaded = false;",
			"a failed refresh keeps the rows and shows a notice"},
		{"      return Applied::Restart;\n", "",
			"a page from another snapshot restarts the list"},
		{"state.next.empty() ? last : std::max<int64_t>(ranked, last)", "std::max<int64_t>(ranked, last)",
			"a board of three spans three"},
		{"    if (lineTop <= y) y = std::max(lineTop, y - lineHeight);",
			"    if (lineTop <= y && lineHeight < 0) y = std::max(lineTop, y - lineHeight);",
			"inside the 201st"},
		{"      if (fleet.incentiveStale) value += ", "      if (false) value += ",
			"every fact, the incentive flagged stale"},
		{"      if (IsSnapshotExpired(outcome.error)) return Applied::Restart;\n", "",
			"a next page whose snapshot expired reloads from the top"},
		{"error.substr(0, 3) == \"410\" && (error[3] == ' ' || error[3] == ':');",
			"error.find(\"410\") != std::string_view::npos;",
			"a next page refused otherwise keeps its retry: Get"},
		{"      card.tone = Tone::Muted;\n    }\n    return card;\n  }", "      card.tone = Tone::Muted;\n    }\n  }",
			"why it cannot be scored"},
	}
	for _, control := range controls {
		requireProtocolBoardFailure(t, func(source string) string {
			if strings.Count(source, control.old) != 1 {
				t.Fatalf("negative control expects exactly one %q in ProtocolBoardPresentation.h", control.old)
			}
			return strings.Replace(source, control.old, control.new, 1)
		}, control.want)
	}
}

// The spec's English is the catalog's: every key it reads, word for word, so
// its cases read what the app shows.
func TestProtocolBoardSpecEnglishIsTheCatalogs(t *testing.T) {
	root := repositoryRoot(t)
	spec, err := os.ReadFile(filepath.Join(root, "app", "tools", "protocol-board-tests.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	table := regexp.MustCompile(`(?s)kEnglish = \{(.*?)\n\};`).FindSubmatch(spec)
	if table == nil {
		t.Fatal("protocol-board-tests.cpp no longer has its kEnglish table; update this contract")
	}
	entry := regexp.MustCompile(`(?s)\{"([a-z0-9_]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}`)
	literal := regexp.MustCompile(`"((?:[^"\\]|\\.)*)"`)
	escape := regexp.MustCompile(`\\x([0-9A-Fa-f]{2})`)
	entries := entry.FindAllSubmatch(table[1], -1)
	if len(entries) < 50 {
		t.Fatalf("read only %d English entries from protocol-board-tests.cpp; update this contract", len(entries))
	}
	for _, match := range entries {
		key := string(match[1])
		english := ""
		for _, part := range literal.FindAllSubmatch(match[2], -1) {
			text := strings.ReplaceAll(string(part[1]), `\"`, `"`)
			// the spec spells its non-ASCII English as UTF-8 bytes (\xC2\xB7)
			text = escape.ReplaceAllStringFunc(text, func(hex string) string {
				var value byte
				for _, c := range hex[2:] {
					value <<= 4
					switch {
					case '0' <= c && c <= '9':
						value |= byte(c - '0')
					case 'a' <= c && c <= 'f':
						value |= byte(c-'a') + 10
					default:
						value |= byte(c-'A') + 10
					}
				}
				return string([]byte{value})
			})
			english += text
		}
		if got := html.UnescapeString(reswValue(t, root, "en", key)); got != english {
			t.Errorf("protocol-board-tests.cpp's English for %s is %q; Strings/en/Resources.resw has %q", key, english, got)
		}
	}
}

// The LEADERTOP keys (CONTRACT.md), every one of which the board or the tile
// shows somewhere.
var protocolBoardStoreKeys = []string{
	"leaderboard_tab_protocol", "leaderboard_protocol_banner_closed", "leaderboard_protocol_banner_stale",
	"leaderboard_protocol_projected", "leaderboard_protocol_of_ranked", "leaderboard_protocol_above_line",
	"leaderboard_protocol_below_line", "leaderboard_protocol_trend_up", "leaderboard_protocol_trend_down",
	"leaderboard_protocol_why", "leaderboard_protocol_solo", "leaderboard_protocol_you",
	"leaderboard_protocol_label_estimate", "leaderboard_protocol_label_likely",
	"leaderboard_protocol_label_borderline", "leaderboard_protocol_label_unlikely",
	"leaderboard_protocol_label_not_measurable", "leaderboard_protocol_chart_title",
	"leaderboard_protocol_chart_line", "leaderboard_protocol_chart_a11y", "leaderboard_protocol_claim_inside",
	"leaderboard_protocol_claim_button", "leaderboard_protocol_claim_soon", "leaderboard_protocol_claim_borderline",
	"leaderboard_protocol_claim_warning", "leaderboard_protocol_not_measurable_cohort",
	"leaderboard_protocol_not_measurable_shared", "leaderboard_protocol_not_measurable_providing",
	"leaderboard_protocol_line", "leaderboard_protocol_line_a11y", "leaderboard_protocol_uid",
	"leaderboard_protocol_explainer", "leaderboard_protocol_empty", "leaderboard_protocol_detail_claimed",
	"leaderboard_protocol_detail_projected_rank", "leaderboard_protocol_detail_validator_rank",
	"leaderboard_protocol_detail_validator_rank_value", "leaderboard_protocol_detail_validator_score",
	"leaderboard_protocol_detail_replica_score", "leaderboard_protocol_detail_incentive",
	"leaderboard_protocol_detail_stale", "leaderboard_protocol_detail_generation",
	"leaderboard_protocol_detail_valid_until", "leaderboard_protocol_detail_epoch",
	"leaderboard_protocol_detail_selected", "leaderboard_protocol_detail_not_selected",
	"leaderboard_protocol_detail_incentive_note", "earnings_top200_projected", "earnings_top200_body",
	"earnings_top200_claiming_soon", "earnings_top200_claiming_soon_body", "earnings_top200_see_board",
}

// Every key the board's sources read is in the catalog, in English and in
// every locale the store translates the screen into, and the sources read
// every LEADERTOP key.
func TestProtocolBoardStringsExist(t *testing.T) {
	root := repositoryRoot(t)
	keyPattern := regexp.MustCompile(`"((?:leaderboard_protocol|leaderboard_tab_protocol|earnings_top200)[a-z0-9_]*)"`)
	keys := map[string]bool{}
	for _, name := range []string{"ProtocolBoardPresentation.h", "ProtocolBoard.cpp", "WalletPage.cpp"} {
		for _, match := range keyPattern.FindAllStringSubmatch(stripComments(readAppSource(t, name)), -1) {
			keys[match[1]] = true
		}
	}
	for _, key := range protocolBoardStoreKeys {
		if !keys[key] {
			t.Errorf("the Protocol board's sources no longer read %s", key)
		}
	}
	for key := range keys {
		for _, locale := range []string{"en", "de", "ja", "ar", "zh-Hans"} {
			if reswValue(t, root, locale, key) == "" {
				t.Errorf("the Protocol board reads %q, which %s/Resources.resw does not define", key, locale)
			}
		}
	}
	// the reused keys, by name (CONTRACT.md: the name switch, close and retry)
	board := stripComments(readAppSource(t, "ProtocolBoard.cpp"))
	for _, key := range []string{"show_on_points_leaderboard", "retry", "close", "something_went_wrong",
		"please_login_to_urnetwork", "leaderboard_position_indicator"} {
		if !strings.Contains(board, `Loc("`+key+`")`) {
			t.Errorf("ProtocolBoard.cpp no longer reads %s", key)
		}
	}
	presentation := stripComments(readAppSource(t, "ProtocolBoardPresentation.h"))
	for _, key := range []string{"anonymous", "top200_bound_detail"} {
		if !strings.Contains(presentation, `"`+key+`"`) {
			t.Errorf("ProtocolBoardPresentation.h no longer reads %s", key)
		}
	}
	// the tile's old reading is gone with its keys (the store moves windows to
	// deprecated for them)
	wallet := stripComments(readAppSource(t, "WalletPage.cpp"))
	for _, key := range []string{"top200_you_qualify", "top200_detail", "top200_bound_status"} {
		if strings.Contains(wallet, `"`+key+`"`) {
			t.Errorf("WalletPage.cpp still reads %s: the tile reads GET /sn/top/me now", key)
		}
	}
}

// The board's requests: the list on the in-process Api with or without a
// session (GET /sn/top is public) but never in --preview-ui, the card only
// with a session; each answer read on the sdk's thread and marshalled
// through the DispatcherQueue behind the window's weak reference and the
// page's alive_ guard, and applied only to the open it went out under and
// only when it is the latest; a page from another snapshot reloads from the
// top; a sign-out resets the board; the tile reads the card's answer.
func TestProtocolBoardWiring(t *testing.T) {
	board := stripComments(readAppSource(t, "ProtocolBoard.cpp"))
	body := func(signature string) string {
		return definitionBody(t, "ProtocolBoard.cpp", board, signature)
	}
	requireAll := func(name, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}

	requireAll("ProtocolBoard::HasSession", body("bool ProtocolBoard::HasSession() const {"),
		"return !w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn();")
	requireAll("ProtocolBoard::CanLoadList", body("bool ProtocolBoard::CanLoadList() const {"),
		"return !w_.previewUi() && Sdk().apiReady();")
	requireSessionsOrder(t, "ProtocolBoard::Reload", sessionsCode(body("void ProtocolBoard::Reload() {")),
		"if(!CanLoadList())return;", "constuint64_tgeneration=model_.Current();",
		"constpb::Model::PageRequestrequest=model_.BeginReload();", "args.limit=request.limit;",
		"Sdk().api().snTop(args,", "pb::TopOutcomeoutcome=pb::TopOutcomeFrom(result,err);",
		"queue.TryEnqueue(", "if(!*alive)return;",
		"self->wallet().protocol().ApplyPage(generation,request,std::move(outcome));")
	requireSessionsOrder(t, "ProtocolBoard::LoadNextPage",
		sessionsCode(body("void ProtocolBoard::LoadNextPage(int64_t limit) {")),
		"if(!CanLoadList())return;", "model_.BeginNextPage(limit);", "if(!request)return;",
		"args.cursor=sent.cursor;", "Sdk().api().snTop(args,", "pb::TopOutcomeFrom(result,err)",
		"if(!*alive)return;", "self->wallet().protocol().ApplyPage(generation,sent,std::move(outcome));")
	requireSessionsOrder(t, "ProtocolBoard::ApplyPage",
		sessionsCode(body("void ProtocolBoard::ApplyPage(uint64_t generation, pb::Model::PageRequest request, pb::TopOutcome outcome) {")),
		"if(!model_.Admits(generation))return;", "model_.ApplyPage(request,std::move(outcome),NowMillis())",
		"casepb::Model::Applied::Dropped:return;", "casepb::Model::Applied::Restart:", "Reload();",
		"Render();", "ContinueSeek();")
	requireSessionsOrder(t, "ProtocolBoard::LoadMe", sessionsCode(body("void ProtocolBoard::LoadMe() {")),
		"if(!HasSession()){", "constuint64_tload=model_.BeginMe();", "Sdk().api().snTopMe(",
		"pb::MeOutcomeoutcome=pb::MeOutcomeFrom(result,err);", "if(!*alive)return;",
		"self->wallet().protocol().ApplyMe(generation,load,std::move(outcome));")
	requireSessionsOrder(t, "ProtocolBoard::ApplyMe",
		sessionsCode(body("void ProtocolBoard::ApplyMe(uint64_t generation, uint64_t load, pb::MeOutcome outcome) {")),
		"if(!model_.Admits(generation))return;", "if(!model_.ApplyMe(load,std::move(outcome)))return;", "Render();")
	requireSessionsOrder(t, "ProtocolBoard::ResetForSignOut", body("void ProtocolBoard::ResetForSignOut() {"),
		"model_.Close();", "model_.Open();", "Render();")
	requireAll("ProtocolBoard::SetShowing", body("void ProtocolBoard::SetShowing(bool showing) {"),
		"model_.ShouldRefresh(NowMillis())", "Reload();", "LoadMe();")
	requireAll("ProtocolBoard::ShowPreview", body("void ProtocolBoard::ShowPreview(bool sample) {"),
		"preview_ = true;", "model_.ApplyPage(model_.BeginReload(), std::move(page), NowMillis());")
	if strings.Contains(body("void ProtocolBoard::ShowPreview(bool sample) {"), "Sdk().api()") {
		t.Error("ProtocolBoard::ShowPreview makes a request: --preview-ui has no session and asks nothing")
	}
	for call, want := range map[string]int{".snTop(": 2, ".snTopMe(": 1, ".snHead(": 0} {
		if got := strings.Count(board, call); got != want {
			t.Errorf("ProtocolBoard.cpp makes %d %s calls, want %d", got, call, want)
		}
	}

	// the thumb's and the chart's pointer: PointerPoint::Position needs the
	// complete Microsoft.UI.Input projection first (the build failure
	// TestWalletPositionIndicatorIncludesCompletePointerPointType records)
	rawBoard := readAppSource(t, "ProtocolBoard.cpp")
	projection := strings.Index(rawBoard, `#include <winrt/Microsoft.UI.Input.h>`)
	pointer := strings.Index(rawBoard, ".GetCurrentPoint(")
	if projection < 0 || pointer < 0 || projection > pointer {
		t.Error("ProtocolBoard.cpp calls PointerPoint::Position without first including the complete Microsoft.UI.Input projection")
	}

	// the tile reads the card's answer, and only with a session
	requireAll("ProtocolBoard::RenderTile", body("void ProtocolBoard::RenderTile() {"),
		"w_.wallet().ApplyTop200(HasSession() ? pb::TileFor(state.me, snapshot, BoardText()) : pb::Tile{});")
	// "Claim on Earnings": the tile when the wallet pane shows it, else the tile's own claim
	requireSessionsOrder(t, "ProtocolBoard::ClaimOnEarnings", body("void ProtocolBoard::ClaimOnEarnings() {"),
		"w_.WalletPaneA().Visibility() == Visibility::Visible", "w_.Top200Panel().StartBringIntoView();",
		"return;", "w_.wallet().OnClaimTop200(nullptr, nullptr);")
	// the claimed row's sheet: one sheet at a time, read from the board's rows
	requireSessionsOrder(t, "ProtocolBoard::ShowDetail",
		body("winrt::fire_and_forget ProtocolBoard::ShowDetail(int64_t position) {"),
		"pb::RowAt(model_.state(), position)", "if (!row || !row->Claimed()) co_return;", "if (w_.sheetOpen()) {",
		"pb::DetailFor(*row, BoardText())", "window->SetSheetOpen(true);", "co_await dialog.ShowAsync();",
		"window->SetSheetOpen(false);")
	// the card's switch is the Points board's: WalletPage writes it
	requireSessionsOrder(t, "ProtocolBoard::OnNamePublicToggled", body("void ProtocolBoard::OnNamePublicToggled() {"),
		"if (applyingNameToggle_ || !nameToggle_) return;", "w_.wallet().RequestNamePublic(requested);")

	// Narrator: the line, the chart, a claimed row and a plain row each read as one sentence
	requireAll("ProtocolBoard::MakeLine", body("UIElement ProtocolBoard::MakeLine(pb::ListItem const& item) {"),
		"Automation::AutomationProperties::SetName(words, Utf8(item.lineSpoken));")
	requireAll("ProtocolBoard::MakeChart", body("FrameworkElement ProtocolBoard::MakeChart(pb::Chart const& chart) {"),
		"Automation::AutomationProperties::SetName(title, Utf8(chart.spoken));", "SetRaw(canvas);")
	makeRow := body("UIElement ProtocolBoard::MakeRow(pb::ListRow const& row) {")
	requireAll("ProtocolBoard::MakeRow", makeRow,
		"Automation::AutomationProperties::SetName(button, Utf8(row.spoken));",
		"Automation::AutomationProperties::SetName(position, Utf8(row.spoken));",
		"ShowDetail(at);", "InsideTint()", "kUnmeasuredGlyph")
	requireAll("ProtocolBoard::BuildIndicator", body("void ProtocolBoard::BuildIndicator() {"),
		`Automation::AutomationProperties::SetName(slider_, Loc("leaderboard_position_indicator"));`)
	// a handler that held its own element would keep every chart a re-render replaces alive
	for _, holder := range []string{"[area, show]", "[area]", "[canvas, area"} {
		if strings.Contains(board, holder) {
			t.Errorf("ProtocolBoard.cpp captures the chart area in its own handler (%s): a cycle that leaks it", holder)
		}
	}

	// the page: three boards, Protocol first and the default
	wallet := stripComments(readAppSource(t, "WalletPage.cpp"))
	walletBody := func(signature string) string {
		return definitionBody(t, "WalletPage.cpp", wallet, signature)
	}
	requireSessionsOrder(t, "WalletPage::ApplyPointsBoardStrings", walletBody("void WalletPage::ApplyPointsBoardStrings() {"),
		`w_.ProtocolBoardItem().Text(Loc("leaderboard_tab_protocol"));`, `w_.PointsBoardItem().Text(Loc("points"));`,
		`w_.DataBoardItem().Text(Loc("data"));`, "w_.LeaderboardBoardBar().SelectedItem(w_.ProtocolBoardItem());",
		"ShowBoard(Board::Protocol);", "protocol_->ApplyStrings();")
	requireAll("WalletPage::ShowBoard", walletBody("void WalletPage::ShowBoard(Board board) {"),
		"w_.ProtocolHost().Visibility(shown(protocol));", "w_.PointsHost().Visibility(shown(pointsBoardShowing_));",
		"w_.LeaderboardDataHost().Visibility(shown(data));", "w_.ProtocolNetworkHost().Visibility(shown(protocol));",
		"w_.PointsNetworkHost().Visibility(shown(pointsBoardShowing_));", "w_.DataRankingHost().Visibility(shown(data));",
		"protocol_->SetShowing(protocol && w_.LeaderboardHost().Visibility() == Visibility::Visible);")
	requireAll("WalletPage::InitializePointsBoard", walletBody("void WalletPage::InitializePointsBoard() {"),
		"{w_.ProtocolBoardItem(), w_.PointsBoardItem(), w_.DataBoardItem()}", "ResetBoardList(board);")
	requireAll("WalletPage::ResetBoardList", walletBody("void WalletPage::ResetBoardList(Board board) {"),
		"protocol_->ScrollToTop();")
	requireAll("WalletPage::OnEarningsTableChanged", walletBody("void WalletPage::OnEarningsTableChanged("),
		"protocol_->SetShowing(leaderboard && board_ == Board::Protocol);")
	requireAll("WalletPage::SetSelected", walletBody("void WalletPage::SetSelected(bool selected) {"),
		"protocol_->SetShowing(selected && board_ == Board::Protocol &&")
	requireAll("WalletPage::LoadWallet", walletBody("void WalletPage::LoadWallet() {"), "protocol_->LoadMe();")
	requireAll("WalletPage::ResetForSignOut", walletBody("void WalletPage::ResetForSignOut() {"),
		"protocol_->ResetForSignOut();")
	requireAll("WalletPage::ShowPreviewLeaderboardState", walletBody("void WalletPage::ShowPreviewLeaderboardState() {"),
		"protocol_->ShowPreview(PreviewSample());")
	if strings.Contains(wallet, ".snHead(") {
		t.Error("WalletPage.cpp still reads GET /sn/head: the tile reads the Protocol card's GET /sn/top/me")
	}
	// the tile: the existing claim flow only while binding is open
	requireAll("WalletPage::ApplyTop200", walletBody("void WalletPage::ApplyTop200(protocolboard::Tile const& tile) {"),
		"tile.body != protocolboard::TileBody::Hidden",
		"w_.Top200Button().Visibility(tile.claimButton ? Visibility::Visible : Visibility::Collapsed);",
		"w_.Top200SignButton().Visibility(tile.signButton ? Visibility::Visible : Visibility::Collapsed);",
		"w_.Top200SoonPanel().Visibility(soon ? Visibility::Visible : Visibility::Collapsed);",
		"w_.Top200Warning().Visibility(tile.nearLine ? Visibility::Visible : Visibility::Collapsed);")
	requireAll("WalletPage::ShowProtocolBoard", walletBody("void WalletPage::ShowProtocolBoard() {"),
		"w_.LeaderboardBoardBar().SelectedItem(w_.ProtocolBoardItem());",
		"w_.EarningsTableBar().SelectedItem(w_.LeaderboardTabItem());")
	// one name switch behind two toggles
	requireSessionsOrder(t, "WalletPage::RequestNamePublic", walletBody("void WalletPage::RequestNamePublic(bool requested) {"),
		"if (settingPointsPublic_) {", "if (!CanCallApi()) {", "settingPointsPublic_ = true;", "SyncNameSwitches();",
		"Sdk().api().setPointsLeaderboardPublic(")
	requireAll("WalletPage::SyncNameSwitches", walletBody("void WalletPage::SyncNameSwitches() {"),
		"SetPointsToggle(pointsPublic_);", "protocol_->ApplyNamePublic(pointsPublic_, settingPointsPublic_);")
	requireAll("WalletPage::ApplyPointsPublicResult", walletBody("void WalletPage::ApplyPointsPublicResult("),
		"protocol_->ReloadAfterNameChange();", "SyncNameSwitches();")
	requireAll("WalletPage::ApplyRankingNamePublic", walletBody("void WalletPage::ApplyRankingNamePublic("),
		"if (askedAt < ownFlagsEditedAt_) return;", "pointsPublic_ = isPublic;", "SyncNameSwitches();")
	requireAll("WalletPage::OnPointsPublicToggled", walletBody("void WalletPage::OnPointsPublicToggled() {"),
		"RequestNamePublic(pointsPublicToggle_.IsOn());")

	// the fold: the card ahead of the list below the three-pane width
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	fold := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyWalletFold(bool earningsThree, bool earningsTwo) {")
	requireSessionsOrder(t, "MainWindow::ApplyWalletFold", fold,
		"FoldBack(ProtocolNetworkHost(), ProtocolCompactHost(), protocolNetworkHome_.parent,",
		"FoldBack(PointsNetworkHost(), fold,",
		"FoldIn(ProtocolNetworkHost(), ProtocolCompactHost(), protocolNetworkHome_.parent);")
	requireAll("MainWindow::RecordFoldHomes", definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::RecordFoldHomes() {"),
		"record(ProtocolNetworkHost(), WalletPaneCStack(), protocolNetworkHome_);")

	// the markup
	markup := readAppSource(t, "MainWindow.xaml")
	for _, name := range []string{"ProtocolBoardItem", "ProtocolHost", "ProtocolScroll", "ProtocolScrollContent",
		"ProtocolCompactHost", "ProtocolNotice", "ProtocolRows", "ProtocolFooterRing", "ProtocolFooterText",
		"ProtocolRetryButton", "ProtocolYouHost", "ProtocolNetworkHost", "Top200ChipHost", "Top200Projected",
		"Top200Fleets", "Top200BoundDetail", "Top200SoonPanel", "Top200SoonTitle", "Top200SoonBody",
		"Top200ClaimWarning", "Top200SeeBoardButton"} {
		if !strings.Contains(markup, `x:Name="`+name+`"`) {
			t.Errorf("MainWindow.xaml: missing %s", name)
		}
	}
	protocol := strings.Index(markup, `<muxc:SelectorBarItem x:Name="ProtocolBoardItem" />`)
	points := strings.Index(markup, `<muxc:SelectorBarItem x:Name="PointsBoardItem" />`)
	data := strings.Index(markup, `<muxc:SelectorBarItem x:Name="DataBoardItem" />`)
	if protocol < 0 || points < 0 || data < 0 || !(protocol < points && points < data) {
		t.Error("MainWindow.xaml: the boards must read Protocol, Points, Data (Q14)")
	}
	networkProtocol := strings.Index(markup, `<StackPanel x:Name="ProtocolNetworkHost" Visibility="Collapsed" />`)
	networkPoints := strings.Index(markup, `<StackPanel x:Name="PointsNetworkHost" Visibility="Collapsed" />`)
	paneC := strings.Index(markup, `<StackPanel x:Name="WalletPaneCStack">`)
	if paneC < 0 || networkProtocol < paneC || networkPoints < networkProtocol {
		t.Error("MainWindow.xaml: the Protocol block opens pane C, ahead of the points block")
	}

	project := readAppSource(t, "App.vcxproj")
	for _, item := range []string{`<ClCompile Include="ProtocolBoard.cpp" />`, `<ClInclude Include="ProtocolBoard.h" />`,
		`<ClInclude Include="ProtocolBoardPresentation.h" />`} {
		if !strings.Contains(project, item) {
			t.Errorf("App.vcxproj does not list %s", item)
		}
	}
}
