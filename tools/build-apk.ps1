# =====================================================================
#  Build APK Android Manggot IoT langsung di laptop (tanpa GitHub)
#  Otomatis memasang: Node.js, Java JDK 17, Android SDK (sekali saja)
#  Hasil: manggot-iot.apk di folder proyek
# =====================================================================
$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Proj  = Split-Path $PSScriptRoot -Parent
$Work  = Join-Path $env:LOCALAPPDATA 'ManggotBuild'
$Sdk   = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$env:CI = '1'                                 # tanpa pertanyaan interaktif dari Capacitor

function Ok($m)   { Write-Host "[OK] $m" -ForegroundColor Green }
function Info($m) { Write-Host "[..] $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "[X] $m" -ForegroundColor Red; Read-Host 'Tekan Enter untuk keluar'; exit 1 }
function RefreshPath { $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User') }

Write-Host '==========================================================' -ForegroundColor Yellow
Write-Host '  BUILD APK ANDROID MANGGOT IoT' -ForegroundColor Yellow
Write-Host '  Pertama kali butuh 10-20 menit (unduh +-1 GB). Berikutnya +-1 menit.' -ForegroundColor Yellow
Write-Host '==========================================================' -ForegroundColor Yellow

# ---------- 1. Node.js ----------
if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
  Info 'Memasang Node.js LTS...'
  winget install --id OpenJS.NodeJS.LTS -e --silent --accept-source-agreements --accept-package-agreements
  RefreshPath
  $env:Path += ';C:\Program Files\nodejs'
}
if (-not (Get-Command node -ErrorAction SilentlyContinue)) { Fail 'Node.js gagal dipasang. Pasang manual: https://nodejs.org (LTS) lalu jalankan lagi.' }
Ok "Node.js $(node -v)"

# ---------- 2. Java JDK 17 ----------
function Find-Jdk17 {
  $cand = @()
  if ($env:JAVA_HOME) { $cand += $env:JAVA_HOME }
  foreach ($root in 'C:\Program Files\Microsoft', 'C:\Program Files\Eclipse Adoptium', 'C:\Program Files\Java', 'C:\Program Files\Zulu') {
    if (Test-Path $root) { $cand += (Get-ChildItem $root -Directory -Filter '*17*' | ForEach-Object FullName) }
  }
  foreach ($c in $cand) {
    $rel = Join-Path $c 'release'
    if ((Test-Path $rel) -and (Select-String -Path $rel -Pattern 'JAVA_VERSION="17' -Quiet)) { return $c }
  }
  return $null
}
$jdk = Find-Jdk17
if (-not $jdk) {
  Info 'Memasang Java JDK 17...'
  winget install --id Microsoft.OpenJDK.17 -e --silent --accept-source-agreements --accept-package-agreements
  $jdk = Find-Jdk17
}
if (-not $jdk) { Fail 'JDK 17 gagal dipasang. Pasang manual: https://learn.microsoft.com/java/openjdk/download (versi 17)' }
$env:JAVA_HOME = $jdk
$env:Path = "$jdk\bin;" + $env:Path
Ok "Java 17: $jdk"

# ---------- 3. Android SDK ----------
$sdkm = Join-Path $Sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
if (-not (Test-Path $sdkm)) {
  Info 'Mengunduh Android command line tools (+-150 MB)...'
  New-Item -ItemType Directory -Force -Path $Sdk | Out-Null
  $zip = Join-Path $env:TEMP 'cmdline-tools.zip'
  Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip' -OutFile $zip
  $tmp = Join-Path $env:TEMP 'cmdline-tools-x'
  if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
  Expand-Archive $zip -DestinationPath $tmp -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $Sdk 'cmdline-tools') | Out-Null
  if (Test-Path (Join-Path $Sdk 'cmdline-tools\latest')) { Remove-Item (Join-Path $Sdk 'cmdline-tools\latest') -Recurse -Force }
  Move-Item (Join-Path $tmp 'cmdline-tools') (Join-Path $Sdk 'cmdline-tools\latest')
  Remove-Item $zip -Force
}
if (-not (Test-Path $sdkm)) { Fail 'Android SDK gagal disiapkan.' }
$env:ANDROID_HOME = $Sdk
$env:ANDROID_SDK_ROOT = $Sdk
if (-not (Test-Path (Join-Path $Sdk 'platforms\android-34'))) {
  Info 'Menerima lisensi & memasang Android SDK 34 (+-300 MB)...'
  ("y`n" * 40) | & $sdkm --sdk_root=$Sdk --licenses | Out-Null
  & $sdkm --sdk_root=$Sdk 'platform-tools' 'platforms;android-34' 'build-tools;34.0.0'
}
if (-not (Test-Path (Join-Path $Sdk 'platforms\android-34'))) { Fail 'Android SDK 34 gagal dipasang. Cek koneksi internet lalu jalankan lagi.' }
Ok "Android SDK: $Sdk"

# ---------- 4. Siapkan proyek Capacitor ----------
Info 'Menyiapkan file aplikasi...'
New-Item -ItemType Directory -Force -Path "$Work\www", "$Work\assets" | Out-Null
Get-ChildItem "$Work\www" -Force | Remove-Item -Recurse -Force
foreach ($f in 'index.html', 'style.css', 'app.js', 'firebase-config.js', 'manifest.json') { Copy-Item "$Proj\$f" "$Work\www\" -Force }
Copy-Item "$Proj\icons" "$Work\www\icons" -Recurse -Force
Copy-Item "$Proj\android-build\package.json", "$Proj\android-build\capacitor.config.json" $Work -Force
Copy-Item "$Proj\android-build\icon-*.png", "$Proj\android-build\splash*.png" "$Work\assets\" -Force
Set-Location $Work

if (-not (Test-Path "$Work\node_modules\@capacitor\android")) {
  Info 'npm install (sekali saja)...'
  npm install --no-audit --no-fund
  if ($LASTEXITCODE -ne 0) { Fail 'npm install gagal. Cek internet lalu jalankan lagi.' }
}
npx cap telemetry off 2>$null | Out-Null
if (-not (Test-Path "$Work\android")) {
  Info 'Membuat proyek Android...'
  npx cap add android
  if ($LASTEXITCODE -ne 0) { Fail 'Gagal membuat proyek Android.' }
  npx capacitor-assets generate --android
}
npx cap sync android
if ($LASTEXITCODE -ne 0) { Fail 'cap sync gagal.' }
Ok 'Proyek Android siap'

# ---------- 5. Versi otomatis ----------
$now = Get-Date
$code = [int]$now.ToString('yyMMddHH')
$name = '1.0.' + $now.ToString('yyMMdd.HHmm')
$gradle = "$Work\android\app\build.gradle"
$g = Get-Content $gradle -Raw
$g = $g -replace 'versionCode \d+', "versionCode $code" -replace 'versionName "[^"]*"', "versionName `"$name`""
[IO.File]::WriteAllText($gradle, $g)
[IO.File]::WriteAllText("$Work\android\local.properties", 'sdk.dir=' + ($Sdk -replace '\\', '\\'))

# ---------- 6. Build ----------
Info 'Build APK (pertama kali Gradle mengunduh +-300 MB)...'
$ks = "$Proj\android-build\debug.keystore"
Set-Location "$Work\android"
& .\gradlew.bat assembleDebug --no-daemon `
  "-Pandroid.injected.signing.store.file=$ks" "-Pandroid.injected.signing.store.password=android" `
  "-Pandroid.injected.signing.key.alias=androiddebugkey" "-Pandroid.injected.signing.key.password=android"
if ($LASTEXITCODE -ne 0) { Fail 'Build APK gagal. Kirim tulisan merah di atas ke Claude.' }

$apk = "$Work\android\app\build\outputs\apk\debug\app-debug.apk"
if (-not (Test-Path $apk)) { Fail 'File APK tidak ditemukan.' }
$out = Join-Path $Proj 'manggot-iot.apk'
Copy-Item $apk $out -Force
Ok "APK jadi: $out  (versi $name)"

# ---------- 7. Pasang langsung jika HP tersambung USB ----------
$adb = Join-Path $Sdk 'platform-tools\adb.exe'
if (Test-Path $adb) {
  $dev = (& $adb devices) | Select-String "`tdevice$"
  if ($dev) {
    Info 'HP terdeteksi lewat USB, memasang APK...'
    & $adb install -r $out
    if ($LASTEXITCODE -eq 0) { Ok 'Aplikasi terpasang di HP' }
  }
}

Write-Host ''
Write-Host '==========================================================' -ForegroundColor Green
Write-Host '  SELESAI!  File: manggot-iot.apk (di folder proyek)' -ForegroundColor Green
Write-Host '  Kirim ke HP (kabel/WhatsApp/Drive), buka, lalu Instal.' -ForegroundColor Green
Write-Host '==========================================================' -ForegroundColor Green
Start-Process explorer.exe "/select,`"$out`""
Read-Host 'Tekan Enter untuk menutup'
