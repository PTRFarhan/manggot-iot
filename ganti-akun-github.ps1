# =====================================================================
#  Ganti akun GitHub: hapus SEMUA login GitHub lama dari Windows,
#  lalu login ulang lewat browser, lalu (opsional) langsung upload.
# =====================================================================
$Expected = 'PTRFarhan'
$Repo     = 'manggot-iot'
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[!]  $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; Read-Host 'Tekan Enter untuk keluar'; exit 1 }

Write-Host '==========================================================' -ForegroundColor Yellow
Write-Host '  GANTI AKUN GITHUB' -ForegroundColor Yellow
Write-Host '==========================================================' -ForegroundColor Yellow
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'Git belum terpasang.' }

# ---------- 1. Hapus semua login lama ----------
Info 'Menghapus semua login GitHub yang tersimpan di Windows...'
$accounts = git credential-manager github list 2>$null
foreach ($a in $accounts) {
  $a = "$a".Trim()
  if ($a) { git credential-manager github logout $a 2>$null | Out-Null; Write-Host "     logout: $a" }
}
for ($i = 0; $i -lt 5; $i++) { "protocol=https`nhost=github.com`n`n" | git credential reject 2>$null }
foreach ($t in 'git:https://github.com', 'LegacyGeneric:target=git:https://github.com', 'gh:github.com') {
  cmdkey /delete:$t 2>$null | Out-Null
}
(cmdkey /list) | Select-String 'github' | ForEach-Object {
  $line = "$_"
  if ($line -match 'Target:\s*(.+)$') { cmdkey /delete:($Matches[1].Trim()) 2>$null | Out-Null }
}
git config --global --unset user.name 2>$null
git config --global --unset user.email 2>$null
git config --global credential.helper manager
git config --global credential.https://github.com.gitHubAuthModes browser
Ok 'Login lama sudah dihapus'

# ---------- 2. Login ulang ----------
Write-Host ''
Write-Host '----------------------------------------------------------' -ForegroundColor Yellow
Write-Host '  Browser akan membuka halaman LOGOUT GitHub.' -ForegroundColor Yellow
Write-Host '  1. Klik "Sign out" (kalau masih login akun lain)' -ForegroundColor Yellow
Write-Host "  2. Login dengan akun $Expected" -ForegroundColor Yellow
Write-Host '  3. Kembali ke jendela ini, tekan Enter' -ForegroundColor Yellow
Write-Host '----------------------------------------------------------' -ForegroundColor Yellow
Start-Process 'https://github.com/logout'
Read-Host 'Sudah login sebagai akun yang benar? Tekan Enter'

function Api($path) {
  Invoke-RestMethod -Uri "https://api.github.com$path" -Headers $script:H -ErrorAction Stop
}
$login = $null
for ($try = 1; $try -le 3 -and -not $login; $try++) {
  Info 'Membuka login GitHub di browser, klik Authorize...'
  $out = "protocol=https`nhost=github.com`n`n" | git credential fill
  $user  = ($out | Where-Object { $_ -like 'username=*' }) -replace '^username=', ''
  $token = ($out | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
  if (-not $token) { Fail 'Login dibatalkan.' }
  $script:H = @{ Authorization = "token $token"; 'User-Agent' = 'manggot'; Accept = 'application/vnd.github+json' }
  try { $login = (Api '/user').login } catch { $login = $null }
  if ($login -and $login -ne $Expected) {
    Warn "Yang login: $login (bukan $Expected)."
    $ans = Read-Host "Pakai akun $login saja? (y = ya, Enter = ulangi login)"
    if ($ans -ne 'y') {
      "protocol=https`nhost=github.com`n`n" | git credential reject
      git credential-manager github logout $login 2>$null | Out-Null
      Start-Process 'https://github.com/logout'
      Read-Host "Logout di browser, login sebagai $Expected, lalu tekan Enter"
      $login = $null
    }
  }
}
if (-not $login) { Fail 'Login gagal.' }
"protocol=https`nhost=github.com`nusername=$user`npassword=$token`n`n" | git credential approve
git config --global user.name $login
git config --global user.email "$login@users.noreply.github.com"
Ok "Sekarang login sebagai $login"

# ---------- 3. Upload (opsional) ----------
Write-Host ''
$go = Read-Host 'Langsung upload proyek ke GitHub sekarang? (Y/n)'
if ($go -eq 'n') { Ok 'Selesai. Untuk upload nanti, klik publish.bat'; Read-Host 'Tekan Enter untuk menutup'; exit 0 }

if (-not (Test-Path '.git')) { git init -q }
git rebase --abort 2>$null
git merge --abort 2>$null
git branch -M main 2>$null
git rm --cached -r -q --ignore-unmatch esp32/inkubator_manggot_iot/secrets.h wokwi/secrets.h platformio/include/secrets.h 2>$null | Out-Null
git add -A
if (git diff --cached --name-only | Where-Object { $_ -like '*secrets.h' }) { git reset -q; Fail 'secrets.h ikut ter-add. Dibatalkan.' }
git commit -q -m "Update $(Get-Date -Format 'yyyy-MM-dd HH:mm')" 2>$null | Out-Null
$owner = if ($login -eq $Expected) { $Expected } else { $login }
git remote remove origin 2>$null
git remote add origin "https://github.com/$owner/$Repo.git"
Info "Upload ke https://github.com/$owner/$Repo ..."
git push -u origin main --force
if ($LASTEXITCODE -ne 0) { Fail 'Upload gagal. Cek apakah repo sudah ada dan akun punya akses.' }
Ok 'Upload berhasil'
Write-Host ''
Write-Host "  Repo : https://github.com/$owner/$Repo" -ForegroundColor Green
Write-Host "  Web  : https://$($owner.ToLower()).github.io/$Repo/  (tunggu 1-2 menit, lalu Ctrl+F5)" -ForegroundColor Green
Read-Host 'Tekan Enter untuk menutup'
