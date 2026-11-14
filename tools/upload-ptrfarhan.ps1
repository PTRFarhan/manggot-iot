# =====================================================================
#  Upload Manggot IoT ke https://github.com/PTRFarhan/manggot-iot
#  1. Hapus login GitHub lama (akun teman) dari Windows
#  2. Login lewat browser sebagai PTRFarhan (tanpa kode verifikasi)
#  3. Upload semua file (kecuali secrets.h), aktifkan GitHub Pages
# =====================================================================
$Owner = 'PTRFarhan'
$Repo  = 'manggot-iot'
$ErrorActionPreference = 'Continue'
Set-Location (Split-Path $PSScriptRoot -Parent)
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[!]  $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; Read-Host 'Tekan Enter untuk keluar'; exit 1 }

Write-Host '==========================================================' -ForegroundColor Yellow
Write-Host "  UPLOAD MANGGOT IoT KE github.com/$Owner/$Repo" -ForegroundColor Yellow
Write-Host '==========================================================' -ForegroundColor Yellow

# ---------- 1. Git ----------
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Info 'Git belum ada, memasang lewat winget...'
  winget install --id Git.Git -e --silent --accept-source-agreements --accept-package-agreements
  $env:Path += ';C:\Program Files\Git\cmd'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'Git gagal dipasang. Pasang manual: https://git-scm.com/download/win' }
Ok 'Git siap'

# ---------- 2. Hapus login lama ----------
Info 'Menghapus login GitHub lama dari Windows...'
git credential-manager github logout 2>$null | Out-Null
"protocol=https`nhost=github.com`n`n" | git credential reject 2>$null
cmdkey /delete:git:https://github.com 2>$null | Out-Null
cmdkey /delete:LegacyGeneric:target=git:https://github.com 2>$null | Out-Null
git config --global --unset user.name 2>$null
git config --global --unset user.email 2>$null
git config --global credential.helper manager
git config --global credential.https://github.com.gitHubAuthModes browser
Ok 'Login lama dihapus'

# ---------- 3. Login sebagai PTRFarhan ----------
function Api($method, $path, $body) {
  $prm = @{ Method = $method; Uri = "https://api.github.com$path"; Headers = $script:H; ErrorAction = 'Stop' }
  if ($body) { $prm.Body = ($body | ConvertTo-Json -Depth 5); $prm.ContentType = 'application/json' }
  Invoke-RestMethod @prm
}

$login = $null
for ($try = 1; $try -le 3 -and $login -ne $Owner; $try++) {
  Write-Host ''
  Write-Host '----------------------------------------------------------' -ForegroundColor Yellow
  Write-Host "  PENTING: di browser harus login sebagai $Owner" -ForegroundColor Yellow
  Write-Host '  Kalau browser masih akun teman: Sign out dulu,' -ForegroundColor Yellow
  Write-Host "  lalu login $Owner, kemudian klik Authorize." -ForegroundColor Yellow
  Write-Host '----------------------------------------------------------' -ForegroundColor Yellow
  Read-Host 'Tekan Enter untuk membuka login GitHub di browser'
  $out = "protocol=https`nhost=github.com`n`n" | git credential fill
  $user  = ($out | Where-Object { $_ -like 'username=*' }) -replace '^username=', ''
  $token = ($out | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
  if (-not $token) { Fail 'Login dibatalkan.' }
  $script:H = @{ Authorization = "token $token"; 'User-Agent' = 'manggot-upload'; Accept = 'application/vnd.github+json' }
  try { $login = (Api GET '/user').login } catch { $login = $null }
  if ($login -ne $Owner) {
    Warn "Yang login: '$login', bukan $Owner. Login dihapus, ulangi dengan akun $Owner."
    "protocol=https`nhost=github.com`n`n" | git credential reject
    git credential-manager github logout 2>$null | Out-Null
    Start-Process 'https://github.com/logout'
    $login = $null
  }
}
if ($login -ne $Owner) { Fail "Gagal login sebagai $Owner." }
"protocol=https`nhost=github.com`nusername=$user`npassword=$token`n`n" | git credential approve
Ok "Login sebagai $Owner"

# ---------- 4. Cek repo ----------
try { Api GET "/repos/$Owner/$Repo" | Out-Null; Ok "Repo $Owner/$Repo ditemukan" }
catch {
  Info "Repo belum ada, membuat $Owner/$Repo ..."
  try { Api POST '/user/repos' @{ name = $Repo; description = 'Monitoring inkubator manggot BSF'; private = $false } | Out-Null; Ok 'Repo dibuat' }
  catch { Fail "Gagal membuat repo: $($_.Exception.Message)" }
}

# ---------- 5. Siapkan git baru (riwayat akun teman dibuang) ----------
if (Test-Path '.git') { Remove-Item '.git' -Recurse -Force }
git init -q
git branch -M main 2>$null
git config user.name $Owner
git config user.email "$Owner@users.noreply.github.com"
git add -A
if (git diff --cached --name-only | Where-Object { $_ -like '*secrets.h' }) {
  git reset -q
  Fail 'secrets.h ikut ter-add. Dibatalkan demi keamanan.'
}
git commit -q -m 'Upload Manggot IoT' | Out-Null
git remote add origin "https://github.com/$Owner/$Repo.git"
Ok 'File siap (secrets.h tidak ikut)'

Info 'Upload ke GitHub...'
git push -u origin main --force
if ($LASTEXITCODE -ne 0) { Fail 'Upload gagal. Cek koneksi internet lalu jalankan lagi.' }
Ok "Terupload: https://github.com/$Owner/$Repo"

# ---------- 6. GitHub Pages ----------
Info 'Mengaktifkan GitHub Pages...'
try { Api POST "/repos/$Owner/$Repo/pages" @{ source = @{ branch = 'main'; path = '/' } } | Out-Null } catch { }
try { Api GET "/repos/$Owner/$Repo/pages" | Out-Null; Ok 'GitHub Pages aktif' }
catch {
  Start-Process "https://github.com/$Owner/$Repo/settings/pages"
  Fail 'Pages belum aktif. Aktifkan manual: Settings > Pages > main / root > Save'
}

$site = "https://$($Owner.ToLower()).github.io/$Repo/"
Info 'Menunggu situs selesai dibangun (biasanya 1-2 menit)...'
for ($i = 1; $i -le 30; $i++) {
  $st = $null
  try { $st = (Api GET "/repos/$Owner/$Repo/pages").status } catch { }
  if ($st -eq 'built') { break }
  Write-Host "     status: $st ($i/30)"
  Start-Sleep -Seconds 10
}

Write-Host ''
Write-Host '==========================================================' -ForegroundColor Green
Write-Host '  SELESAI!' -ForegroundColor Green
Write-Host "  Aplikasi : $site"
Write-Host "  Repo     : https://github.com/$Owner/$Repo"
Write-Host '  Login di aplikasi dengan EMAIL KAMU (bukan esp32).'
Write-Host '  Update berikutnya cukup klik publish.bat'
Write-Host '==========================================================' -ForegroundColor Green
Start-Process $site
Read-Host 'Tekan Enter untuk menutup'
