# Run by `cmake --install` on Windows: a Start menu shortcut for Explorer, and
# the install folder on the user's PATH for a shell and an agent.
param([Parameter(Mandatory)][string]$AppDir)

$link = Join-Path ([Environment]::GetFolderPath('Programs')) 'clap-host.lnk'
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($link)
$shortcut.TargetPath = Join-Path $AppDir 'clap-host.exe'
$shortcut.WorkingDirectory = $AppDir
$shortcut.Save()

$path = [Environment]::GetEnvironmentVariable('Path', 'User')
$entries = @(if ($path) { $path -split ';' | Where-Object { $_ } })
if ($entries -notcontains $AppDir) {
    [Environment]::SetEnvironmentVariable('Path', (($entries + $AppDir) -join ';'), 'User')
}
