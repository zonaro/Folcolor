<#
.SYNOPSIS
Builds the Foldrion Release Win32 target and launches the generated executable.

.DESCRIPTION
Resolves the project paths from the workspace root, finds MSBuild using common
Visual Studio discovery mechanisms, rebuilds the Controller project in
Release|Win32, and starts the generated Foldrion.exe.
#>

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

function Get-MSBuildPath {
    [CmdletBinding()]
    param()

    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $msbuildPath = & $vswherePath -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($msbuildPath) {
            return $msbuildPath
        }
    }

    $command = Get-Command -Name 'msbuild.exe' -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $fallbackPaths = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
    )

    foreach ($candidate in $fallbackPaths) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw 'MSBuild.exe nao foi encontrado. Instale o Visual Studio Build Tools ou ajuste o PATH.'
}

function Stop-FoldrionProcess {
    [CmdletBinding()]
    param()

    $runningProcesses = Get-Process -Name 'Foldrion' -ErrorAction SilentlyContinue
    if (-not $runningProcesses) {
        return
    }

    Write-Host '[CLEANUP] Encerrando instancias do Foldrion antes do build...' -ForegroundColor Yellow
    foreach ($process in $runningProcesses) {
        try {
            Stop-Process -Id $process.Id -Force -ErrorAction Stop
        }
        catch {
            Write-Warning ("Nao foi possivel encerrar Foldrion PID {0}: {1}" -f $process.Id, $_.Exception.Message)
        }
    }
}

try {
    $workspaceRoot = Split-Path -Parent $PSCommandPath
    $projectPath = Join-Path $workspaceRoot 'src\Controller\Controller.vcxproj'
    $exePath = Join-Path $workspaceRoot 'src\Controller\Win32\Release\Foldrion.exe'
    $buildTempPath = Join-Path $workspaceRoot '.build-temp'
    $msbuildPath = Get-MSBuildPath

    if (-not (Test-Path -LiteralPath $projectPath)) {
        throw "Projeto nao encontrado em $projectPath"
    }

    if (-not (Test-Path -LiteralPath $buildTempPath)) {
        $null = New-Item -ItemType Directory -Path $buildTempPath
    }

    $originalTemp = $env:TEMP
    $originalTmp = $env:TMP
    $env:TEMP = $buildTempPath
    $env:TMP = $buildTempPath

    Stop-FoldrionProcess

    Write-Host '[BUILD] Compilando Release Win32...' -ForegroundColor Cyan
    & $msbuildPath $projectPath '/p:Configuration=Release' '/p:Platform=Win32' '/t:Rebuild' '/verbosity:minimal'
    if ($LASTEXITCODE -ne 0) {
        throw "Build falhou com codigo $LASTEXITCODE"
    }

    if (-not (Test-Path -LiteralPath $exePath)) {
        throw "Executavel nao encontrado em $exePath"
    }  
  
    Write-Host '[OK] Build concluido.' -ForegroundColor Green

    # Write version to version.txt
    Push-Location (Split-Path -Parent $exePath)
    & $exePath --version 

    #delete build temp directory, and foldrion.pdb if it exists
    $buildPath = Join-Path (Split-Path -Parent $exePath) "build"
    if (Test-Path -LiteralPath $buildPath) {
        Remove-Item -Path $buildPath -Recurse -Force
        Write-Host '[CLEANUP] Build temp directory removed.' -ForegroundColor Green
    }
    $pdbPath = Join-Path (Split-Path -Parent $exePath) "Foldrion.pdb"
    if (Test-Path -LiteralPath $pdbPath) {
        Remove-Item -Path $pdbPath -Force
        Write-Host '[CLEANUP] PDB file removed.' -ForegroundColor Green
    }
  
}
catch {
    Write-Error $_
    exit 1
}
finally {
    if ($null -ne $originalTemp) {
        $env:TEMP = $originalTemp
    }

    if ($null -ne $originalTmp) {
        $env:TMP = $originalTmp
    }
}
