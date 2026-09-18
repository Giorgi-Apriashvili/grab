# Adds a directory to the current user's PATH (HKCU\Environment) if it is not already
# there. Run by `cmake --install` on Windows; safe to run repeatedly.
#
# Works on the registry value directly rather than through
# [Environment]::SetEnvironmentVariable so that existing %VAR% references in PATH are kept
# unexpanded, and never uses setx, which truncates PATH at 1024 characters.
param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
)

$ErrorActionPreference = 'Stop'

$dir = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\')
$key = Get-Item -Path 'HKCU:\Environment'
$current = [string]$key.GetValue('Path', '', 'DoNotExpandEnvironmentNames')
$entries = @($current -split ';' | Where-Object { $_ -ne '' })
$expanded = @($entries | ForEach-Object {
    [System.Environment]::ExpandEnvironmentVariables($_).TrimEnd('\')
})

if ($expanded -contains $dir) {
    Write-Output "user PATH already contains $dir"
    exit 0
}

$new = ($entries + $dir) -join ';'
Set-ItemProperty -Path 'HKCU:\Environment' -Name 'Path' -Value $new -Type ExpandString

# Broadcast WM_SETTINGCHANGE so Explorer and newly opened terminals see the new PATH.
# Deleting a never-set user variable is a no-op that still triggers the broadcast.
[System.Environment]::SetEnvironmentVariable('GRAB_PATH_REFRESH', $null, 'User')

Write-Output "added $dir to the user PATH; open a new terminal to use grab"
