# A second reader for the same question: WPF's GlyphTypeface reads each font's
# cmap through its own parser (CharacterToGlyphMap), independent of fontTools.
# For every symbol/icon font font_coverage.py found, and every glyph the app
# uses, both readers must agree; a disagreement fails the step.
# Pure ASCII, Windows PowerShell 5.1. Throwaway (a ci/wfix-* branch).
param([Parameter(Mandatory = $true)] [string]$Coverage)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationCore

$data = Get-Content -Raw -LiteralPath $Coverage | ConvertFrom-Json
$needles = @($data.needles | ForEach-Object { [Convert]::ToInt32($_.Substring(2), 16) })
$disagree = 0
foreach ($row in $data.fonts) {
  if ($row.file -match '\.ttc$') { "  $($row.file): a collection, not read here"; continue }
  $typeface = New-Object System.Windows.Media.GlyphTypeface ([Uri]("file:///" + ($row.path -replace '\\', '/')))
  $map = $typeface.CharacterToGlyphMap
  $covered = @($needles | Where-Object { $map.ContainsKey($_) } | ForEach-Object { 'U+{0:X4}' -f $_ })
  $fromFontTools = @($row.covered)
  $only = @(Compare-Object -ReferenceObject $fromFontTools -DifferenceObject $covered -PassThru)
  $state = 'agrees'
  if ($only.Count -gt 0) { $state = "DISAGREES on $($only -join ' ')"; $disagree++ }
  "  $($row.file) '$($typeface.FamilyNames['en-us'])' v$($typeface.Version): GlyphTypeface maps $($covered.Count) of $($needles.Count); fontTools $($fromFontTools.Count); $state"
}
if ($disagree -gt 0) { throw "the two cmap readers disagree on $disagree font(s)" }
