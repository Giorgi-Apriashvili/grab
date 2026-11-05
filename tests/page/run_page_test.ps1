#Requires -Version 7
# Page-logic checks for the window (see README.md): copies the page (src/gui/ui) into a scratch
# folder with test.js loaded after app.js and the CSP meta removed, opens it in a headless Microsoft
# Edge with a profile of its own on a free DevTools port, reads the results (#test-out) over
# DevTools, then closes that Edge (only the processes started with this profile).
# Prints the failures, the INFO lines and one summary line. Exit code: 0 every check passed,
# 1 a check failed or no result, 77 skipped (no Microsoft Edge).
[CmdletBinding()]
param(
    # Scratch folder for the page, the Edge profile and the screenshots; kept afterwards (the
    # profile in it is removed). Default: a new folder in the temp folder, removed afterwards.
    [string]$Work,
    # Keep the default scratch folder: the page and the screenshots shot-1250.png, shot-716.png.
    [switch]$Keep,
    # Print every check, not only the failures and the INFO lines.
    [switch]$Full,
    # The msedge.exe to use instead of the one found.
    [string]$Edge
)
$ErrorActionPreference = 'Stop'
$clock = [Diagnostics.Stopwatch]::StartNew()
$resultWait = 180 # seconds for test.js to finish: the chunk C timings run first

function Find-Edge {
    $candidates = [Collections.Generic.List[string]]::new()
    foreach ($root in ${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:ProgramW6432) {
        if ($root) { $candidates.Add((Join-Path $root 'Microsoft\Edge\Application\msedge.exe')) }
    }
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\msedge.exe') {
        try {
            $value = (Get-ItemProperty -LiteralPath $key -ErrorAction Stop).'(default)'
            if ($value) { $candidates.Add($value.Trim('"')) }
        } catch {}
    }
    $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

function Remove-Folder([string]$path) {
    # Edge lets go of its profile's files a moment after it exits.
    for ($i = 0; $i -lt 20 -and (Test-Path -LiteralPath $path); $i++) {
        try { Remove-Item -LiteralPath $path -Recurse -Force } catch { Start-Sleep -Milliseconds 250 }
    }
    if (Test-Path -LiteralPath $path) { Write-Warning "could not remove $path" }
}

function Invoke-Cdp([string]$method, [hashtable]$params) {
    $id = ++$script:cdpId
    $bytes = [Text.Encoding]::UTF8.GetBytes((@{ id = $id; method = $method; params = $params } | ConvertTo-Json -Depth 6 -Compress))
    $cts = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(30))
    try {
        [void]$script:ws.SendAsync([ArraySegment[byte]]::new($bytes), [Net.WebSockets.WebSocketMessageType]::Text, $true, $cts.Token).GetAwaiter().GetResult()
        while ($true) {
            # Whole messages before decoding: a UTF-8 character can span two reads.
            $message = [IO.MemoryStream]::new()
            do {
                $read = $script:ws.ReceiveAsync([ArraySegment[byte]]::new($script:buffer), $cts.Token).GetAwaiter().GetResult()
                if ($read.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) { throw 'Edge closed the DevTools connection' }
                $message.Write($script:buffer, 0, $read.Count)
            } while (-not $read.EndOfMessage)
            $reply = [Text.Encoding]::UTF8.GetString($message.ToArray()) | ConvertFrom-Json
            if ($reply.id -eq $id) { break } # anything else is an event
        }
    } finally { $cts.Dispose() }
    if ($reply.error) { throw "DevTools $method failed: $($reply.error.message)" }
    $reply.result
}

function Invoke-Page([string]$expression) {
    (Invoke-Cdp 'Runtime.evaluate' @{ expression = $expression; returnByValue = $true }).result.value
}

function Seconds { '{0:0.0} s' -f $clock.Elapsed.TotalSeconds }

$ui = [IO.Path]::GetFullPath([IO.Path]::Combine($PSScriptRoot, '..', '..', 'src', 'gui', 'ui'))
$edgeExe = if ($Edge) { $Edge } else { Find-Edge }
if (-not $edgeExe -or -not (Test-Path -LiteralPath $edgeExe -PathType Leaf)) {
    Write-Host 'page-logic: SKIPPED: Microsoft Edge (msedge.exe) not found; pass -Edge <path to msedge.exe> to name one'
    exit 77
}
$edgeVersion = (Get-Item -LiteralPath $edgeExe).VersionInfo.ProductVersion

$ownWork = -not $Work
$Work = if ($ownWork) {
    Join-Path ([IO.Path]::GetTempPath()) "grab-page-test-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
} else {
    $PSCmdlet.GetUnresolvedProviderPathFromPSPath($Work)
}
$keepWork = $Keep -or -not $ownWork
# A profile name of its own, so the clean-up below never touches another run's Edge.
$profileName = "edge-profile-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
$profileDir = Join-Path $Work $profileName
$proc = $null
$edgeOut = $null
$edgeErr = $null
$ws = $null
$buffer = [byte[]]::new(1MB)
$cdpId = 0
$code = 1
$summary = $null
try {
    # The page: everything in src/gui/ui, index.html with test.js after app.js and without the CSP
    # (its script-src 'self' does not cover a file:// page).
    $page = Join-Path $Work 'page'
    if (Test-Path -LiteralPath $page) { Remove-Item -LiteralPath $page -Recurse -Force }
    New-Item -ItemType Directory -Force $page | Out-Null
    Get-ChildItem -LiteralPath $ui | Copy-Item -Destination $page -Recurse
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test.js') -Destination $page
    $html = [IO.File]::ReadAllText((Join-Path $ui 'index.html'))
    $html = $html -replace '<meta http-equiv="Content-Security-Policy"[^>]*>', ''
    $appTag = '<script src="app.js"></script>'
    if (-not $html.Contains($appTag)) { throw "src/gui/ui/index.html has no $appTag" }
    $html = $html.Replace($appTag, "$appTag`n<script src=`"test.js`"></script>")
    [IO.File]::WriteAllText((Join-Path $page 'index.html'), $html, [Text.UTF8Encoding]::new($false))
    $url = ([Uri](Join-Path $page 'index.html')).AbsoluteUri

    # Port 0: Edge takes a free port and writes it to DevToolsActivePort in its profile.
    $edgeArgs = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--disable-sync',
        '--disable-extensions', '--disable-background-networking', '--disable-component-update',
        '--enable-logging=stderr', '--enable-precise-memory-info', '--remote-debugging-port=0', "--user-data-dir=$profileDir",
        '--window-size=1400,900', $url)
    $start = [Diagnostics.ProcessStartInfo]::new($edgeExe)
    foreach ($arg in $edgeArgs) { $start.ArgumentList.Add($arg) }
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true # both read to the end, so Edge never waits on a full pipe
    $start.RedirectStandardError = $true
    $proc = [Diagnostics.Process]::Start($start)
    $edgeOut = $proc.StandardOutput.ReadToEndAsync() # not shown
    $edgeErr = $proc.StandardError.ReadToEndAsync() # shown when the run fails
    $portFile = Join-Path $profileDir 'DevToolsActivePort'
    $port = 0
    $until = $clock.Elapsed.TotalSeconds + 60
    while (-not $port) {
        # An exit with 0 can be a hand-over (Edge swapping in a staged update relaunches itself).
        if ($proc.HasExited -and $proc.ExitCode -ne 0) { throw "Edge exited (code $($proc.ExitCode)) before it opened DevTools" }
        if ($clock.Elapsed.TotalSeconds -gt $until) { throw 'Edge did not open DevTools within 60 s' }
        Start-Sleep -Milliseconds 100
        try { [void][int]::TryParse((Get-Content -LiteralPath $portFile -TotalCount 1), [ref]$port) } catch {}
    }
    $target = $null
    while (-not $target) {
        if ($clock.Elapsed.TotalSeconds -gt $until) { throw "Edge on port $port shows no page target for $url" }
        try {
            $targets = Invoke-RestMethod -NoProxy -TimeoutSec 5 -Uri "http://127.0.0.1:$port/json/list" # one array, not enumerated
            $target = $targets | Where-Object { $_.type -eq 'page' -and $_.url -like '*/page/index.html' } | Select-Object -First 1
        } catch {}
        if (-not $target) { Start-Sleep -Milliseconds 100 }
    }

    $ws = [Net.WebSockets.ClientWebSocket]::new()
    $ws.Options.Proxy = $null # straight to 127.0.0.1, even with HTTP_PROXY set
    $connect = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(30))
    try { [void]$ws.ConnectAsync([Uri]$target.webSocketDebuggerUrl, $connect.Token).GetAwaiter().GetResult() } finally { $connect.Dispose() }
    $result = $null
    $until = $clock.Elapsed.TotalSeconds + $resultWait
    while (-not $result -and $clock.Elapsed.TotalSeconds -lt $until) {
        $result = Invoke-Page '(() => { const e = document.getElementById("test-out"); return e ? e.textContent : null; })()'
        if (-not $result) { Start-Sleep -Milliseconds 200 }
    }

    if ($result -and $keepWork) {
        # Screenshots of the final state at two widths, for a look.
        try {
            foreach ($w in 1250, 716) {
                [void](Invoke-Page "(() => { const p = document.getElementById('downloads'); p.style.width = '${w}px'; updateDlStep(); renderDlBody(); return true; })()")
                Start-Sleep -Milliseconds 300
                $shot = (Invoke-Cdp 'Page.captureScreenshot' @{ format = 'png' }).data
                if ($shot) { [IO.File]::WriteAllBytes((Join-Path $Work "shot-$w.png"), [Convert]::FromBase64String($shot)) }
            }
        } catch { Write-Warning "no screenshots: $_" }
    }

    if (-not $result) {
        $summary = "FAILED: no result from test.js within $resultWait s (did app.js or test.js throw before it finished?)"
    } else {
        $lines = $result -split "`r?`n"
        $passed = @($lines | Where-Object { $_ -like 'PASS *' }).Count
        $failed = @($lines | Where-Object { $_ -like 'FAIL *' }).Count
        $thrown = @($lines | Where-Object { $_ -like 'EXCEPTION *' }).Count
        # The first line is test.js's own verdict: ALL PASS (0) or FAILED (<count>).
        foreach ($line in $lines | Select-Object -Skip 1) {
            if ($Full -or $line -notlike 'PASS *') { Write-Host $line }
        }
        $total = $passed + $failed
        if ($lines[0] -eq 'ALL PASS (0)' -and $failed -eq 0 -and $thrown -eq 0 -and $passed -gt 0) {
            $summary = "all $total checks passed"
            $code = 0
        } else {
            $summary = "FAILED: $failed of $total checks failed" + $(if ($thrown) { ", test.js threw" } else { '' })
        }
    }
} catch {
    $summary = "FAILED: $_"
} finally {
    if ($ws) { $ws.Dispose() }
    if ($proc -and -not $proc.HasExited) { try { $proc.Kill($true) } catch {} }
    Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -like "*$profileName*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if ($code -ne 0 -and $edgeErr -and $edgeErr.Wait(2000) -and $edgeErr.Result.Trim()) {
        Write-Host "Edge's stderr (last lines):"
        $edgeErr.Result.Trim() -split "`r?`n" | Select-Object -Last 20 | ForEach-Object { Write-Host "  $_" }
    }
    Remove-Folder $profileDir
    if ($keepWork) { Write-Host "page-logic: scratch folder kept: $Work" } else { Remove-Folder $Work }
}
Write-Host "page-logic: $summary (Edge $edgeVersion, $(Seconds))"
exit $code
