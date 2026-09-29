# Token-by-token perplexity cells for --expert-substitute (see findings.md).
# Windows host: build\bin\Release on PATH for the DLLs, bmoe-cli.exe from the host build.
$root = "<path to the repository>"
$env:PATH = "$root\build\bin\Release;$env:PATH"
$cli = "$root\build\cli\Release\bmoe-cli.exe"
$model = "<path to>\Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf"
$here = $PSScriptRoot
$common = @("-m", $model, "-t", "8", "-c", "512", "--ubatch", "512", "--moe-stream", "--cache-mb", "2000", "--io-threads", "4", "--overlap")

$cells = @(
  @("b", "0"), @("b", "0.15"), @("b", "0.30"), @("b", "0.60"),
  @("c", "0"), @("c", "0.15"), @("c", "0.30")
)
foreach ($cell in $cells) {
  $txt = $cell[0]; $lam = $cell[1]
  $log = "$here\step_${txt}_L$lam.log"
  if (Test-Path $log) { continue }
  $args = $common + @("--ppl", "$here\text-$txt-heldout.txt", "--ppl-step")
  if ($lam -ne "0") { $args += @("--expert-substitute", $lam) }
  "=== step text=$txt L=$lam" | Out-File -Encoding utf8 $log
  & $cli @args 2>&1 | Out-File -Encoding utf8 -Append $log
}

# Generation cells: same flags, 64 tokens, one prompt.
foreach ($lam in @("0", "0.15")) {
  $log = "$here\gen_L$lam.log"
  if (Test-Path $log) { continue }
  $args = $common + @("-n", "64", "--chatml", "-p", "Explain in a few sentences why solid-state storage changed mobile computing.")
  if ($lam -ne "0") { $args += @("--expert-substitute", $lam) }
  "=== gen L=$lam" | Out-File -Encoding utf8 $log
  & $cli @args 2>&1 | Out-File -Encoding utf8 -Append $log
}
