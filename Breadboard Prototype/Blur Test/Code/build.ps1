param([string]$IdfProfile = 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1')
$ErrorActionPreference = 'Stop'
. $IdfProfile
$env:IDF_CCACHE_ENABLE = '0'
idf.py -DIDF_TARGET=esp32p4 build
exit $LASTEXITCODE
