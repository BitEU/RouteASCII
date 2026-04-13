param(
    [ValidateSet('prep', 'start', 'stop', 'status', 'monitor', 'all')]
    [string]$Action = 'all',
    [string]$PbfPath = 'us-260408.osm.pbf',
    [ValidateSet('car', 'bicycle', 'foot')]
    [string]$RoutingProfile = 'car',
    [string]$DataDir = 'data/osrm-local',
    [string]$Image = 'ghcr.io/project-osrm/osrm-backend:latest',
    [string]$ContainerName = 'routeascii-osrm',
    [int]$Port = 5000,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot

function Assert-Docker {
    docker --version *> $null
    if ($LASTEXITCODE -ne 0) {
        throw 'Docker is required. Install Docker Desktop and ensure docker is on PATH.'
    }
}

function Resolve-InputPbf {
    if ([System.IO.Path]::IsPathRooted($PbfPath)) {
        $candidate = $PbfPath
    } else {
        $candidate = Join-Path $repoRoot $PbfPath
    }

    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "PBF file not found: $candidate"
    }

    return (Resolve-Path -LiteralPath $candidate).Path
}

function Get-BaseName([string]$fileName) {
    return [System.IO.Path]::GetFileNameWithoutExtension($fileName)
}

function Invoke-OsrmContainer([string[]]$arguments) {
    docker run --rm -t -v "${dataDirAbs}:/data" $Image @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "OSRM command failed: $($arguments -join ' ')"
    }
}

function Initialize-PreprocessedData {
    $inputPbf = Resolve-InputPbf
    $pbfName = [System.IO.Path]::GetFileName($inputPbf)

    if (-not (Test-Path -LiteralPath $dataDirAbs)) {
        New-Item -ItemType Directory -Path $dataDirAbs | Out-Null
    }

    $stagedPbf = Join-Path $dataDirAbs $pbfName
    if ($Force -or -not (Test-Path -LiteralPath $stagedPbf)) {
        Copy-Item -LiteralPath $inputPbf -Destination $stagedPbf -Force
        Write-Host "[osrm-local] staged PBF: $stagedPbf"
    } else {
        Write-Host "[osrm-local] using existing staged PBF: $stagedPbf"
    }

    $datasetBase = Get-BaseName $pbfName
    $osrmFile = Join-Path $dataDirAbs "$datasetBase.osrm"

    if ($Force -or -not (Test-Path -LiteralPath $osrmFile)) {
        Write-Host '[osrm-local] extracting graph...'
        Invoke-OsrmContainer @('osrm-extract', '-p', "/opt/$RoutingProfile.lua", "/data/$pbfName")

        Write-Host '[osrm-local] partitioning graph (MLD)...'
        Invoke-OsrmContainer @('osrm-partition', "/data/$datasetBase.osrm")

        Write-Host '[osrm-local] customizing graph...'
        Invoke-OsrmContainer @('osrm-customize', "/data/$datasetBase.osrm")

        Write-Host "[osrm-local] ready: $osrmFile"
    } else {
        Write-Host "[osrm-local] preprocessed dataset already present: $osrmFile"
    }

    return $datasetBase
}

function Start-Osrm([string]$datasetBase) {
    $existing = docker ps -a --filter "name=^/$ContainerName$" --format '{{.ID}}'
    if ($existing) {
        docker rm -f $ContainerName | Out-Null
    }

    docker run -d --name $ContainerName -p "${Port}:5000" -v "${dataDirAbs}:/data" $Image `
        osrm-routed --algorithm mld "/data/$datasetBase.osrm" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to start osrm-routed container.'
    }

    Write-Host "[osrm-local] running at http://127.0.0.1:$Port"
    Write-Host '[osrm-local] tip: set ROUTEASCII_OSRM_URL to this endpoint if using a non-default port.'
}

function Stop-Osrm {
    $existing = docker ps -a --filter "name=^/$ContainerName$" --format '{{.ID}}'
    if ($existing) {
        docker rm -f $ContainerName | Out-Null
        Write-Host '[osrm-local] container stopped and removed.'
    } else {
        Write-Host '[osrm-local] container is not running.'
    }
}

function Show-Status {
    docker ps --filter "name=^/$ContainerName$"
    Write-Host "[osrm-local] expected endpoint: http://127.0.0.1:$Port"
}

function Show-Monitor {
    $refreshSeconds = 10
    while ($true) {
        Clear-Host
        Write-Host "[osrm-local] monitor ($ContainerName)"
        Write-Host "[osrm-local] data dir: $dataDirAbs"
        Write-Host ""

        $cid = docker ps -a --filter "name=^/$ContainerName$" --format '{{.ID}}' | Select-Object -First 1
        if (-not $cid) {
            $cid = docker ps --filter "ancestor=$Image" --format '{{.ID}}' | Select-Object -First 1
        }

        if ($cid) {
            docker ps -a --filter "id=$cid" --format 'Status: {{.Status}}  Name: {{.Names}}  Command: {{.Command}}'
            Write-Host ""
            docker stats --no-stream --format 'CPU={{.CPUPerc}} MEM={{.MemUsage}} NET={{.NetIO}}' $cid
            Write-Host ""
            Write-Host '[osrm-local] recent logs:'
            docker logs --tail 20 $cid
        } else {
            Write-Host '[osrm-local] no matching OSRM container found yet.'
        }

        Write-Host ""
        Write-Host '[osrm-local] generated files:'
        Get-ChildItem -Path $dataDirAbs -Filter '*.osrm*' -ErrorAction SilentlyContinue |
            Sort-Object Name |
            Select-Object Name,
                          @{Name='GB';Expression={[math]::Round($_.Length / 1GB, 3)}},
                          LastWriteTime |
            Format-Table -AutoSize

        Write-Host ""
        Write-Host "[osrm-local] refreshing every $refreshSeconds seconds (Ctrl+C to exit)"
        Start-Sleep -Seconds $refreshSeconds
    }
}

Assert-Docker
$dataDirAbs = (Resolve-Path -LiteralPath (Join-Path $repoRoot $DataDir) -ErrorAction SilentlyContinue)
if (-not $dataDirAbs) {
    New-Item -ItemType Directory -Path (Join-Path $repoRoot $DataDir) | Out-Null
    $dataDirAbs = (Resolve-Path -LiteralPath (Join-Path $repoRoot $DataDir)).Path
} else {
    $dataDirAbs = $dataDirAbs.Path
}

switch ($Action) {
    'prep' {
        [void](Initialize-PreprocessedData)
    }
    'start' {
        $base = Initialize-PreprocessedData
        Start-Osrm -datasetBase $base
    }
    'stop' {
        Stop-Osrm
    }
    'status' {
        Show-Status
    }
    'monitor' {
        Show-Monitor
    }
    'all' {
        $base = Initialize-PreprocessedData
        Start-Osrm -datasetBase $base
        Show-Status
    }
}
