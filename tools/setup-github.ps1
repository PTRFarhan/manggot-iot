# =====================================================================
#  Setup otomatis Manggot IoT ke GitHub
#  Login lewat browser (Git Credential Manager), tanpa kode verifikasi.
#  Lalu: buat repo, upload, aktifkan GitHub Pages, buka aplikasi.
# =====================================================================
$Repo = 'manggot-iot'          # nama repo, boleh diganti
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; Read-Host 'Tekan Enter untuk keluar'; exit 1 }

Write-Host '==========================================================' -ForegroundColor Yellow
Write-Host '  SETUP OTOMATIS MANGGOT IoT KE GITHUB' -ForegroundColor Yellow
Write-Host '==========================================================' -ForegroundColor Yellow

# ---------- 1. Git ----------
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Info 'Git belum ada, memasang lewat winget...'
  winget install --id Git.Git -e --silent --accept-source-agreements --accept-package-agreements
  $env:Path += ';C:\Program Files\Git\cmd'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'Git gagal dipasang. Pasang manual: https://git-scm.com/download/win' }
Ok 'Git siap'

# Git Credential Manager: login lewat browser
git config --global credential.helper manager
git config --global credential.https://github.com.gitHubAuthModes browser

# ---------- 2. Login GitHub ----------
function Get-Token {
  $out = "protocol=https`nhost=github.com`n`n" | git credential fill
  $u = ($out | Where-Object { $_ -like 'username=*' }) -replace '^username=', ''
  $p = ($out | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
  return @($u, $p)
}
function Api($method, $path, $body) {
  $prm = @{ Method = $method; Uri = "https://api.github.com$path"; Headers = $script:H; ErrorAction = 'Stop' }
  if ($body) { $prm.Body = ($body | ConvertTo-Json -Depth 5); $prm.ContentType = 'application/json' }
  Invoke-RestMethod @prm
}

$me = $null
for ($try = 1; $try -le 2 -and -not $me; $try++) {
  Info 'Login GitHub: browser akan terbuka, login lalu klik Authorize...'
  $cred = Get-Token
  if (-not $cred[1]) { Fail 'Login dibatalkan.' }
  $script:H = @{ Authorization = "token $($cred[1])"; 'User-Agent' = 'manggot-setup'; Accept = 'application/vnd.github+json' }
  try { $me = Api GET '/user' }
  catch {
    Info 'Login lama sudah tidak berlaku, login ulang...'
    "protocol=https`nhost=github.com`n`n" | git credential reject
  }
}
if (-not $me) { Fail 'Login GitHub gagal.' }
"protocol=https`nhost=github.com`nusername=$($cred[0])`npassword=$($cred[1])`n`n" | git credential approve
$login = $me.login
Ok "Login sebagai $login"

# ---------- 3. Identitas commit ----------
if (-not (git config user.email)) {
  git config --global user.name $login
  git config --global user.email "$login@users.noreply.github.com"
}

# ---------- 4. Repo lokal ----------
if (-not (Test-Path '.git')) { git init -q }
git branch -M main 2>$null
git rm --cached -r -q --ignore-unmatch esp32/inkubator_manggot_iot/secrets.h wokwi/secrets.h 2>$null | Out-Null
git add -A
if (git diff --cached --name-only | Where-Object { $_ -like '*secrets.h' }) {
  git reset -q
  Fail 'secrets.h ikut ter-add. Dibatalkan demi keamanan.'
}
git commit -q -m 'Upload Manggot IoT' 2>$null | Out-Null
Ok 'File siap diupload (secrets.h tidak ikut)'

# ---------- 5. Repo GitHub ----------
$exists = $true
try { Api GET "/repos/$login/$Repo" | Out-Null } catch { $exists = $false }
if (-not $exists) {
  Info "Membuat repo publik $login/$Repo ..."
  try { Api POST '/user/repos' @{ name = $Repo; description = 'Monitoring inkubator manggot BSF'; private = $false } | Out-Null }
  catch { Fail "Gagal membuat repo: $($_.Exception.Message)" }
  Ok 'Repo dibuat'
} else { Info "Repo $login/$Repo sudah ada, upload ke sana" }

git remote remove origin 2>$null
git remote add origin "https://github.com/$login/$Repo.git"
Info 'Upload ke GitHub...'
git rebase --abort 2>$null
# isi folder ini dianggap versi paling benar (menimpa isi repo di GitHub)
git push -u origin main --force
if ($LASTEXITCODE -ne 0) { Fail 'Upload gagal.' }
Ok "Kode terupload: https://github.com/$login/$Repo"

# ---------- 6. GitHub Pages ----------
Info 'Mengaktifkan GitHub Pages...'
try { Api POST "/repos/$login/$Repo/pages" @{ source = @{ branch = 'main'; path = '/' } } | Out-Null } catch { }
try { Api GET "/repos/$login/$Repo/pages" | Out-Null; Ok 'GitHub Pages aktif' }
catch {
  Start-Process "https://github.com/$login/$Repo/settings/pages"
  Fail 'Pages belum aktif. Aktifkan manual: Settings > Pages > main / root > Save'
}

$site = "https://$($login.ToLower()).github.io/$Repo/"
Info 'Menunggu situs selesai dibangun (biasanya 1-2 menit)...'
for ($i = 1; $i -le 30; $i++) {
  $st = $null
  try { $st = (Api GET "/repos/$login/$Repo/pages").status } catch { }
  if ($st -eq 'built') { break }
  Write-Host "     status: $st ($i/30)"
  Start-Sleep -Seconds 10
}

Write-Host ''
Write-Host '==========================================================' -ForegroundColor Green
Write-Host '  SELESAI!' -ForegroundColor Green
Write-Host "  Aplikasi : $site"
Write-Host "  Repo     : https://github.com/$login/$Repo"
Write-Host '  Login di aplikasi dengan EMAIL KAMU (bukan esp32).'
Write-Host '  Update berikutnya cukup klik publish.bat'
Write-Host '==========================================================' -ForegroundColor Green
Start-Process $site
Read-Host 'Tekan Enter untuk menutup'
