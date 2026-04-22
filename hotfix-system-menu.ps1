param([int]$PageSize = 40)

$hkcuRoot = 'HKCU:\Software\Classes\Directory\shell\Folcolor'
$hkcrRoot = 'HKCR:\Directory\shell\Folcolor'
$sysSubKey = 'shell\0001\shell'

function Copy-RegistryTree($src, $dst) {
    if (!(Test-Path $dst)) { New-Item -Path $dst -Force | Out-Null }
    $srcProps = Get-ItemProperty $src -ErrorAction SilentlyContinue
    if ($srcProps) {
        foreach ($name in ($srcProps.PSObject.Properties | Where-Object { $_.Name -notlike 'PS*' } | Select-Object -ExpandProperty Name)) {
            Set-ItemProperty -Path $dst -Name $name -Value $srcProps.$name -ErrorAction SilentlyContinue
        }
    }
    Get-ChildItem $src -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-RegistryTree $_.PSPath (Join-Path $dst $_.PSChildName)
    }
}

Write-Host '[1] Limpando HKCU antiga...'
Remove-Item $hkcuRoot -Recurse -Force -ErrorAction SilentlyContinue

Write-Host '[2] Copiando HKCR -> HKCU...'
Copy-RegistryTree $hkcrRoot $hkcuRoot

$sysShellPath = Join-Path $hkcuRoot $sysSubKey
if (!(Test-Path $sysShellPath)) { Write-Host 'ERRO: System shell nao encontrado'; exit 1 }

Write-Host "[3] Paginando com PageSize=$PageSize..."
$dllKeys = Get-ChildItem $sysShellPath | Sort-Object PSChildName

foreach ($dllKey in $dllKeys) {
    $dllProps = Get-ItemProperty $dllKey.PSPath -ErrorAction SilentlyContinue
    $dllLabel = [string]$dllProps.MUIVerb
    $dllShell = Join-Path $dllKey.PSPath 'shell'
    if (!(Test-Path $dllShell)) { continue }
    $iconItems = Get-ChildItem $dllShell | Sort-Object PSChildName
    $total = $iconItems.Count
    if ($total -le $PageSize) { Write-Host "  $dllLabel : $total items ok"; continue }
    Write-Host "  $dllLabel : $total items -> paginando..."
    $itemsData = @()
    foreach ($item in $iconItems) {
        $ip = Get-ItemProperty $item.PSPath -ErrorAction SilentlyContinue
        $cmdKey = Join-Path $item.PSPath 'command'
        $cmdVal = ''
        if (Test-Path $cmdKey) { $cmdVal = [string](Get-ItemProperty $cmdKey -ErrorAction SilentlyContinue).'(default)' }
        $itemsData += [PSCustomObject]@{ MUIVerb = $([string]$ip.MUIVerb); Icon = $([string]$ip.Icon); Command = $cmdVal }
    }
    Get-ChildItem $dllShell | ForEach-Object { Remove-Item $_.PSPath -Recurse -Force }
    $pageCount = [math]::Ceiling($total / $PageSize)
    for ($page = 0; $page -lt $pageCount; $page++) {
        $firstIdx = $page * $PageSize
        $lastIdx = [math]::Min($firstIdx + $PageSize - 1, $total - 1)
        $pageLabel = "$firstIdx-$lastIdx"
        $pagePath = Join-Path $dllShell ('{0:D4}' -f $page)
        New-Item -Path $pagePath -Force | Out-Null
        Set-ItemProperty -Path $pagePath -Name 'MUIVerb'     -Value $pageLabel
        Set-ItemProperty -Path $pagePath -Name 'Icon'        -Value $itemsData[$firstIdx].Icon
        Set-ItemProperty -Path $pagePath -Name 'SubCommands' -Value ''
        $pageShell = Join-Path $pagePath 'shell'
        New-Item -Path $pageShell -Force | Out-Null
        $itemOrder = 0
        for ($idx = $firstIdx; $idx -le $lastIdx; $idx++) {
            $d = $itemsData[$idx]
            $ip2 = Join-Path $pageShell ('{0:D4}' -f $itemOrder)
            New-Item -Path $ip2 -Force | Out-Null
            Set-ItemProperty -Path $ip2 -Name 'MUIVerb' -Value $d.MUIVerb
            Set-ItemProperty -Path $ip2 -Name 'Icon'    -Value $d.Icon
            $cp2 = Join-Path $ip2 'command'
            New-Item -Path $cp2 -Force | Out-Null
            Set-ItemProperty -Path $cp2 -Name '(default)' -Value $d.Command
            $itemOrder++
        }
    }
    Write-Host "    -> $pageCount paginas"
}

Write-Host '[4] Reiniciando Explorer...'
Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Process explorer.exe
Write-Host 'Concluido!'
