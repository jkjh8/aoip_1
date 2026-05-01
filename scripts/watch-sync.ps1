# Windows FileSystemWatcher → WSL rsync 자동 동기화
param(
    [string]$WatchPath = "C:\Users\kjh\Desktop\DEV\aoip_1"
)

$Remote = "kjh@192.168.10.97"
$RemoteDir = "/home/kjh/aoip_1/"
$LocalDir = "/mnt/c/Users/kjh/Desktop/DEV/aoip_1/"

$Excludes = "--exclude=.git/ --exclude=.claude/ --exclude=node_modules/ --exclude=.vscode/ --exclude=*.log --exclude=config/channels.json"

# 디바운스: 연속 저장 시 마지막 한 번만 실행
$script:debounceTimer = $null
$script:pendingSync = $false

function Invoke-Sync {
    $timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host "[$timestamp] 동기화 중..." -ForegroundColor Cyan
    $cmd = "rsync -az $Excludes $LocalDir ${Remote}:${RemoteDir}"
    $result = wsl -e bash -c $cmd 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[$timestamp] 완료" -ForegroundColor Green
    } else {
        Write-Host "[$timestamp] 실패: $result" -ForegroundColor Red
    }
}

# 감시 제외 패턴
$ExcludeDirs = @('.git', 'node_modules', '.claude', 'logs')

$watcher = New-Object System.IO.FileSystemWatcher
$watcher.Path = $WatchPath
$watcher.IncludeSubdirectories = $true
$watcher.EnableRaisingEvents = $true
$watcher.NotifyFilter = [System.IO.NotifyFilters]::LastWrite -bor
                        [System.IO.NotifyFilters]::FileName -bor
                        [System.IO.NotifyFilters]::DirectoryName

$action = {
    $path = $Event.SourceEventArgs.FullPath

    # 제외 경로 필터
    foreach ($dir in $ExcludeDirs) {
        if ($path -like "*\$dir\*" -or $path -like "*\$dir") { return }
    }
    if ($path -like "*.log") { return }

    # 디바운스 300ms
    if ($script:debounceTimer) {
        $script:debounceTimer.Stop()
        $script:debounceTimer.Dispose()
    }
    $script:debounceTimer = New-Object System.Timers.Timer
    $script:debounceTimer.Interval = 300
    $script:debounceTimer.AutoReset = $false
    Register-ObjectEvent -InputObject $script:debounceTimer -EventName Elapsed -Action {
        Invoke-Sync
    } | Out-Null
    $script:debounceTimer.Start()
}

Register-ObjectEvent $watcher "Changed" -Action $action | Out-Null
Register-ObjectEvent $watcher "Created" -Action $action | Out-Null
Register-ObjectEvent $watcher "Deleted" -Action $action | Out-Null
Register-ObjectEvent $watcher "Renamed" -Action $action | Out-Null

Write-Host "===================================" -ForegroundColor Yellow
Write-Host " Watch & Sync 시작" -ForegroundColor Yellow
Write-Host " 감시: $WatchPath" -ForegroundColor Yellow
Write-Host " 대상: ${Remote}:${RemoteDir}" -ForegroundColor Yellow
Write-Host " 중지: Ctrl+C" -ForegroundColor Yellow
Write-Host "===================================" -ForegroundColor Yellow

# 시작 시 1회 전체 동기화
Invoke-Sync

# 이벤트 루프
try {
    while ($true) { Start-Sleep -Seconds 1 }
} finally {
    $watcher.EnableRaisingEvents = $false
    $watcher.Dispose()
    Write-Host "Watch-Sync 종료" -ForegroundColor Yellow
}
