param(
    [ValidateSet('Start','Stop','Status','Test')][string]$Action='Status',
    [string]$Config='',
    [string]$Text='猫猫，我今天有点累，请简短地安慰我。',
    [switch]$ServerOnly,
    [switch]$NoBuild
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if (-not $Config) { $Config=Join-Path $root 'server/config.local.json' }
$work=Join-Path $root 'test-results/local-server'
$stateFile=Join-Path $work 'process.json'
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Get-OwnedProcess {
    if (-not (Test-Path -LiteralPath $stateFile)) { return $null }
    $state=Get-Content -Raw -LiteralPath $stateFile | ConvertFrom-Json
    $proc=Get-Process -Id $state.pid -ErrorAction SilentlyContinue
    if ($proc -and $proc.Path -eq $state.exe -and $proc.StartTime.ToUniversalTime().Ticks -eq ([datetime]$state.started).ToUniversalTime().Ticks) { return $proc }
    return $null
}
function Read-Config {
    if (-not (Test-Path -LiteralPath $Config)) { throw "Copy server/config.example.json to $Config and configure it locally first." }
    $script:cfg=Get-Content -Raw -LiteralPath $Config -Encoding UTF8 | ConvertFrom-Json
    # No authentication is provided yet: never silently expose this control API to a LAN/public interface.
    if ($cfg.listen -notmatch '^127\.0\.0\.1:\d+$') { throw 'Local deployment requires a 127.0.0.1 listen address.' }
}
function Get-Go {
    if ($cfg.go) { $p=$cfg.go; if (-not [IO.Path]::IsPathRooted($p)) { $p=Join-Path $root $p }; return (Resolve-Path -LiteralPath $p).Path }
    $bundled=Join-Path $PSScriptRoot '.venv/go-runtime/go/bin/go.exe'
    if (Test-Path -LiteralPath $bundled) { return $bundled }
    return (Get-Command go -ErrorAction Stop).Source
}
function Build-Go([string]$package,[string]$output) {
    $env:GOCACHE=Join-Path $root 'test-results/go-cache'
    $env:GOTMPDIR=Join-Path $root 'test-results/go-tmp'
    New-Item -ItemType Directory -Force -Path $env:GOCACHE,$env:GOTMPDIR | Out-Null
    & (Get-Go) -C (Join-Path $root 'server') build -o $output $package
    if ($LASTEXITCODE -ne 0) { throw "Go build failed: $package" }
}

if ($Action -eq 'Stop') {
    $proc=Get-OwnedProcess
    if ($proc) { Stop-Process -Id $proc.Id; $proc.WaitForExit(5000) | Out-Null; Write-Output 'Stopped this workspace server.' }
    else { Write-Output 'No owned server is running.' }
    exit 0
}
Read-Config
$url='http://'+$cfg.listen
if ($Action -eq 'Status') {
    $proc=Get-OwnedProcess
    if (-not $proc) { Write-Output 'No owned server is running.'; exit 1 }
    Invoke-RestMethod -Uri "$url/healthz" -TimeoutSec 3 | ConvertTo-Json
    Write-Output "PID: $($proc.Id)"
    exit 0
}
if ($Action -eq 'Test') {
    if (-not (Get-OwnedProcess)) { throw 'Start this workspace server first.' }
    $tester=Join-Path $work 'meoave-local-test.exe'
    Build-Go './cmd/meoave-local-test' $tester
    $report=Join-Path $work ('acceptance-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'.json')
    $play=if ($ServerOnly) { 'false' } else { 'true' }
    & $tester -server $url "-play=$play" -text $Text -report $report
    exit $LASTEXITCODE
}
if (Get-OwnedProcess) { Write-Output 'Already running; use Status, Test or Stop.'; exit 0 }
$port=[int]($cfg.listen.Split(':')[-1])
if (Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue) { throw "Port $port is owned by another process; nothing was stopped." }
$devicePort=''
if (-not $ServerOnly -and $cfg.device_port) {
    $ports=@(Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -like 'USB\VID_303A&PID_4005&MI_00\*' })
    if ($cfg.device_port -eq 'auto') {
        if ($ports.Count -ne 1) { throw 'Expected exactly one P4 application native USB CDC port; no reset or fallback to UART performed.' }
        $devicePort=$ports[0].DeviceID
    } else {
        if ($cfg.device_port -notin $ports.DeviceID) { throw 'Configured port is not a verified P4 native USB application port.' }
        $devicePort=$cfg.device_port
    }
}
$exe=Join-Path $work 'meoave-server.exe'
if (-not $NoBuild -or -not (Test-Path -LiteralPath $exe)) { Build-Go './cmd/meoave-server' $exe }
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$out=Join-Path $work "$stamp.out.log"; $err=Join-Path $work "$stamp.err.log"
$names=@('MEOAVE_LLM_API_KEY','MEOAVE_LLM_BASE_URL','MEOAVE_LLM_MODEL','MEOAVE_LLM_REASONING_EFFORT','MEOAVE_LLM_JSON_OUTPUT')
$saved=@{}
foreach($name in $names){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try {
    if ($cfg.llm_api_key -and -not $env:MEOAVE_LLM_API_KEY) { $env:MEOAVE_LLM_API_KEY=$cfg.llm_api_key }
    if ($cfg.llm_base_url) { $env:MEOAVE_LLM_BASE_URL=$cfg.llm_base_url }
    if ($cfg.llm_model) { $env:MEOAVE_LLM_MODEL=$cfg.llm_model }
    if ($cfg.llm_reasoning_effort) { $env:MEOAVE_LLM_REASONING_EFFORT=$cfg.llm_reasoning_effort }
    $env:MEOAVE_LLM_JSON_OUTPUT=if ($cfg.llm_json_output) { 'true' } else { 'false' }
    $arguments=@('-listen',$cfg.listen,'-voice',$cfg.tts_voice)
    if ($devicePort) { $arguments+=@('-device-port',$devicePort,'-device-baud',"$($cfg.device_baud)") }
    $proc=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    @{pid=$proc.Id;exe=$exe;started=$proc.StartTime.ToUniversalTime().ToString('o');url=$url;stdout=$out;stderr=$err;port=$devicePort} | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
} finally { foreach($name in $names){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')} }
$healthy=$false
for ($i=0;$i -lt 20;$i++) {
    try { $health=Invoke-RestMethod -Uri "$url/healthz" -TimeoutSec 1; $healthy=$true; break } catch { Start-Sleep -Milliseconds 150 }
}
if (-not $healthy) { if (Get-OwnedProcess) { Stop-Process -Id $proc.Id }; throw "Server did not become healthy; see $err" }
$health | ConvertTo-Json
Write-Output "Running on this computer, PID $($proc.Id); logs: $out"
