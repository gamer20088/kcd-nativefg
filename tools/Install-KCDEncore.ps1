<#
.SYNOPSIS
    KCD NativeFG installer: Kingdom Come: Deliverance + RTX Encore 2.5.0 + KCD NativeFG (dinput8.dll).

.DESCRIPTION
    Install   Downloads RTX Encore 2.5.0 and NVIDIA Streamline SDK 2.14.16 from their official GitHub releases,
              verifies every hash, downloads this add-on's release, backs up what it replaces and installs
              into Bin\Win64. Writes the saved profiles (kcd_clarity.ini, rtx-encore.jsonc).
    Update    Same, but keeps the existing configuration files.
    Rollback  Undoes the last Install/Update (restores replaced files, removes files it added).
    Uninstall Removes every file this installer owns (only if unchanged since it installed it) and restores what was
              there before the first install. Unrelated mods and files are never touched.
    Status    Shows what is installed and whether it matches the pinned hashes.

    State lives in Bin\Win64\.kcd-encore\ (manifest.json + one folder per transaction).

.EXAMPLE
    .\Install-KCDEncore.ps1                      # install, game folder found through Steam
    .\Install-KCDEncore.ps1 -Action Update -Release v1.0.0   
    .\Install-KCDEncore.ps1 -Action Rollback
    .\Install-KCDEncore.ps1 -Action Uninstall
#>
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Update', 'Rollback', 'Uninstall', 'Status')]
    [string]$Action = 'Install',
    [string]$GameDir,                                   # ...\KingdomComeDeliverance (auto-detected if omitted)
    [string]$Release = 'v1.2.0',                        # this add-on's GitHub release tag
    [string]$Repo = 'gamer20088/kcd-nativefg',
    [string]$ModDir,                                    # local folder with dinput8.dll (+ profiles) instead of gh
    [string]$CacheDir = (Join-Path $env:LOCALAPPDATA 'KCDEncore\cache'),
    [switch]$Force                                      # replace a foreign dinput8.dll/version.dll (backed up)
)
$ErrorActionPreference = 'Stop'

# ---- Pinned sources and hashes ----------------------------------------------------------------------------------------
$Encore = @{
    Url  = 'https://github.com/SilyNoMeta/rtx-encore/releases/download/v2.5.0/RTX-Encore-v2.5.0.zip'
    Zip  = '249a22ff63bbbeedc29d1509c1f7bb21570e18cf09a1ae3eeba775855beacbda'
    Dll  = '58fff4489e2222cb263e17142613693869acec458e0fa0b7ab260482fbf8c762'   # rtx-encore.dll -> version.dll
}
$Streamline = @{
    Url = 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip'
    Zip = '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
    Files = [ordered]@{   # verified in game with Encore 2.5.0 (Encore logs them as admitted)
        'sl.interposer.dll' = 'b1d7395aebd7b9722841d64dcf7433680020b6a8e421f851c16d5ba0c2182edf'
        'sl.common.dll'     = '38eea3b4209afc455636ea372b5933d65fcc13becec6c84d101dcfc4d8648f75'
        'sl.dlss.dll'       = '18871eea87c90345da97d8109faa81ea7d5840e8d99b51b55b0e46cfa177f84e'
        'sl.dlss_g.dll'     = '3ed9538df724993bea8a050195a6a78b9c8c8a566d5b05ade477aab3c390e90f'
        'sl.pcl.dll'        = '5698c38e722318681f215c9fe68e45b9bedd51378acc699d8eee26e961e57770'
        'sl.reflex.dll'     = '443577e869352436ff7ec8f4d9c90418d3e95368436ce219261e0f4f07a38122'
        'nvngx_dlss.dll'    = 'd00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e'
        'nvngx_dlssg.dll'   = '8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7'
    }
}
$ModFiles = @('dinput8.dll')
$ConfigFiles = @('kcd_clarity.ini', 'rtx-encore.jsonc')
$KcdAppId = '379430'

function Write-Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Get-Sha($p) { (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash.ToLowerInvariant() }

# ---- Game folder --------------------------------------------------------------------------------------------------------
function Find-GameDir {
    $roots = @()
    foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        $p = (Get-ItemProperty -Path $k -ErrorAction SilentlyContinue)
        if ($p.SteamPath) { $roots += ($p.SteamPath -replace '/', '\') }
        if ($p.InstallPath) { $roots += $p.InstallPath }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $acf = Join-Path $l "steamapps\appmanifest_$KcdAppId.acf"
        if (Test-Path $acf) {
            $dir = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"').Groups[1].Value
            $g = Join-Path $l "steamapps\common\$dir"
            if (Test-Path (Join-Path $g 'Bin\Win64\KingdomCome.exe')) { return $g }
        }
    }
    throw 'Kingdom Come: Deliverance not found through Steam. Pass -GameDir <...\KingdomComeDeliverance>.'
}

# ---- Downloads ----------------------------------------------------------------------------------------------------------
function Get-Verified($url, $sha) {
    New-Item -ItemType Directory -Force $CacheDir | Out-Null
    $file = Join-Path $CacheDir ([IO.Path]::GetFileName($url))
    if ((Test-Path $file) -and (Get-Sha $file) -eq $sha) { Write-Host "    cached $file"; return $file }
    Write-Step "Downloading $url"
    $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
    try { Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing } finally { $ProgressPreference = $old }
    $h = Get-Sha $file
    if ($h -ne $sha) { Remove-Item $file -Force; throw "Hash mismatch for $url (got $h, expected $sha)" }
    return $file
}

function Expand-To($zip) {
    $dest = Join-Path $CacheDir ([IO.Path]::GetFileNameWithoutExtension($zip))
    if (-not (Test-Path $dest)) {
        Write-Step "Extracting $([IO.Path]::GetFileName($zip))"
        Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
    }
    return $dest
}

# Returns a hashtable name -> source path, every file checked against its pinned hash.
function Get-Payload {
    $payload = [ordered]@{}
    $enc = Expand-To (Get-Verified $Encore.Url $Encore.Zip)
    $dll = Get-ChildItem $enc -Recurse -Filter 'rtx-encore.dll' | Where-Object { (Get-Sha $_.FullName) -eq $Encore.Dll } |
        Select-Object -First 1
    if (-not $dll) { throw 'rtx-encore.dll with the pinned hash not found in the Encore release.' }
    $payload['version.dll'] = $dll.FullName

    $sl = Expand-To (Get-Verified $Streamline.Url $Streamline.Zip)
    foreach ($name in $Streamline.Files.Keys) {
        $want = $Streamline.Files[$name]
        $hit = Get-ChildItem $sl -Recurse -Filter $name | Where-Object { (Get-Sha $_.FullName) -eq $want } | Select-Object -First 1
        if (-not $hit) { throw "$name with the pinned hash not found in Streamline SDK 2.14.16." }
        $payload[$name] = $hit.FullName
    }

    if ($ModDir) {
        $src = (Resolve-Path $ModDir).Path
    } else {
        $src = Join-Path $CacheDir "release-$Release"
        New-Item -ItemType Directory -Force $src | Out-Null
        Write-Step "Downloading release $Release from $Repo"
        $old = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        try {
            foreach ($f in $ModFiles + $ConfigFiles) {
                $u = "https://github.com/$Repo/releases/download/$Release/$f"
                try { Invoke-WebRequest -Uri $u -OutFile (Join-Path $src $f) -UseBasicParsing }
                catch { if ($ModFiles -contains $f) { throw "Download failed: $u ($($_.Exception.Message))" } }
            }
        } finally { $ProgressPreference = $old }
    }    foreach ($f in $ModFiles + $ConfigFiles) {
        $p = Join-Path $src $f
        if (Test-Path $p) { $payload[$f] = $p } elseif ($ModFiles -contains $f) { throw "$f missing in $src" }
    }
    return $payload
}

# ---- State: manifest + transactions -------------------------------------------------------------------------------------
function Get-State($bin) {
    $dir = Join-Path $bin '.kcd-encore'
    $mf = Join-Path $dir 'manifest.json'
    $m = if (Test-Path $mf) { Get-Content $mf -Raw | ConvertFrom-Json } else { $null }
    $files = @{}
    if ($m) { foreach ($p in $m.files.PSObject.Properties) { $files[$p.Name] = $p.Value } }
    $tx = @(); if ($m -and $m.transactions) { $tx = @($m.transactions) }
    return @{ Dir = $dir; Path = $mf; Files = $files; Tx = $tx }
}
function Save-State($s) {
    New-Item -ItemType Directory -Force $s.Dir | Out-Null
    [pscustomobject]@{ files = $s.Files; transactions = $s.Tx } | ConvertTo-Json -Depth 6 | Set-Content $s.Path -Encoding UTF8
}

function Assert-GameClosed {
    if (Get-Process KingdomCome -ErrorAction SilentlyContinue) { throw 'Close Kingdom Come: Deliverance first.' }
}

function Invoke-Install($bin, [bool]$keepConfig) {
    $s = Get-State $bin
    $payload = Get-Payload
    $known = @{}; foreach ($k in $Streamline.Files.Keys) { $known[$k] = $Streamline.Files[$k] }; $known['version.dll'] = $Encore.Dll
    # Refuse to replace another mod's proxy DLL unless -Force.
    foreach ($proxy in 'version.dll', 'dinput8.dll') {
        $t = Join-Path $bin $proxy
        if ((Test-Path $t) -and -not $s.Files.ContainsKey($proxy) -and -not $Force) {
            $h = Get-Sha $t
            if ($known[$proxy] -ne $h -and (Get-Sha $payload[$proxy]) -ne $h) {
                throw "$proxy in $bin belongs to another mod. Remove it or rerun with -Force (it will be backed up)."
            }
        }
    }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $txDir = Join-Path $s.Dir "tx\$stamp"
    New-Item -ItemType Directory -Force $txDir | Out-Null
    $created = @(); $replaced = @()
    $before = @{}; foreach ($k in $s.Files.Keys) { $before[$k] = $s.Files[$k] }
    foreach ($name in $payload.Keys) {
        $t = Join-Path $bin $name
        if ($ConfigFiles -contains $name -and $keepConfig -and (Test-Path $t)) { Write-Host "    keep $name"; continue }
        if (Test-Path $t) {
            if ((Get-Sha $t) -eq (Get-Sha $payload[$name])) { $s.Files[$name] = Get-Sha $t; continue }
            Copy-Item -LiteralPath $t (Join-Path $txDir $name) -Force
            $replaced += $name
        } else { $created += $name }
        Copy-Item -LiteralPath $payload[$name] $t -Force
        $s.Files[$name] = Get-Sha $t
        Write-Host "    $name"
    }
    $s.Tx = @($s.Tx) + [pscustomobject]@{ id = $stamp; release = $Release; created = $created; replaced = $replaced; before = $before }
    Save-State $s
    Write-Step "Done: $($created.Count) added, $($replaced.Count) replaced (backups in $txDir)"
}

function Invoke-Rollback($bin) {
    $s = Get-State $bin
    if (-not $s.Tx.Count) { throw 'Nothing to roll back.' }
    $last = $s.Tx[-1]
    $txDir = Join-Path $s.Dir "tx\$($last.id)"
    foreach ($n in @($last.created)) {
        $t = Join-Path $bin $n
        if ((Test-Path $t) -and (Get-Sha $t) -eq $s.Files[$n]) { Remove-Item -LiteralPath $t -Force; Write-Host "    removed $n" }
        elseif (Test-Path $t) { Write-Warning "$n changed since install; left in place" }
    }
    foreach ($n in @($last.replaced)) {
        Copy-Item -LiteralPath (Join-Path $txDir $n) (Join-Path $bin $n) -Force; Write-Host "    restored $n"
    }
    $s.Files = @{}; if ($last.before) { foreach ($p in $last.before.PSObject.Properties) { $s.Files[$p.Name] = $p.Value } }
    $s.Tx = @($s.Tx | Select-Object -SkipLast 1)
    Save-State $s
    Write-Step "Rolled back $($last.id)"
}

function Invoke-Uninstall($bin) {
    $s = Get-State $bin
    while ($s.Tx.Count) { Invoke-Rollback $bin; $s = Get-State $bin }   # unwind every transaction, oldest state last
    foreach ($n in @($s.Files.Keys)) {
        $t = Join-Path $bin $n
        if ((Test-Path $t) -and (Get-Sha $t) -eq $s.Files[$n]) { Remove-Item -LiteralPath $t -Force; Write-Host "    removed $n" }
    }
    if (Test-Path $s.Dir) { Remove-Item -LiteralPath $s.Dir -Recurse -Force }
    Write-Step 'Uninstalled. Logs (kcd_clarity.log, rtx-encore-logs) were left in place.'
}

function Show-Status($bin) {
    $s = Get-State $bin
    $pins = @{}; foreach ($k in $Streamline.Files.Keys) { $pins[$k] = $Streamline.Files[$k] }; $pins['version.dll'] = $Encore.Dll
    foreach ($n in @('version.dll') + @($Streamline.Files.Keys) + $ModFiles + $ConfigFiles) {
        $t = Join-Path $bin $n
        $h = if (Test-Path $t) { Get-Sha $t } else { $null }
        $state = if (-not $h) { 'missing' } elseif ($pins[$n] -and $pins[$n] -eq $h) { 'pinned' } elseif ($pins[$n]) { 'DIFFERENT' } else { 'present' }
        $own = if ($s.Files.ContainsKey($n)) { 'owned' } else { '' }
        '{0,-20} {1,-9} {2}' -f $n, $state, $own
    }
    "transactions: $(@($s.Tx | ForEach-Object { $_.id }) -join ', ')"
}

# ---- Main ---------------------------------------------------------------------------------------------------------------
if (-not $GameDir) { $GameDir = Find-GameDir }
$bin = Join-Path $GameDir 'Bin\Win64'
if (-not (Test-Path (Join-Path $bin 'KingdomCome.exe'))) { throw "KingdomCome.exe not found in $bin" }
Write-Step "$Action in $bin"
switch ($Action) {
    'Status'    { Show-Status $bin }
    'Install'   { Assert-GameClosed; Invoke-Install $bin $false }
    'Update'    { Assert-GameClosed; Invoke-Install $bin $true }
    'Rollback'  { Assert-GameClosed; Invoke-Rollback $bin }
    'Uninstall' { Assert-GameClosed; Invoke-Uninstall $bin }
}
