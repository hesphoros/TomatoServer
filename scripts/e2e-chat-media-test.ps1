param(
  [string]$BaseUrl = "https://127.0.0.1:8443"
)

$ErrorActionPreference = "Stop"

function Invoke-ApiJson {
  param(
    [string]$Method,
    [string]$Url,
    [hashtable]$Headers = @{},
    [object]$Body = $null
  )

  if ($null -eq $Body) {
    return Invoke-RestMethod -SkipCertificateCheck -Method $Method -Uri $Url -Headers $Headers
  }

  $jsonBody = if ($Body -is [string]) { $Body } else { $Body | ConvertTo-Json -Depth 10 }
  return Invoke-RestMethod -SkipCertificateCheck -Method $Method -Uri $Url -Headers $Headers -ContentType "application/json" -Body $jsonBody
}

function Assert-True {
  param(
    [bool]$Condition,
    [string]$Message
  )
  if (-not $Condition) {
    throw "ASSERT FAILED: $Message"
  }
}

Write-Host "== Step 0: health check =="
$health = Invoke-RestMethod -SkipCertificateCheck "$BaseUrl/healthz"
Assert-True ($health.status -eq "ok") "healthz must be ok"

Write-Host "== Step 1: login two users =="
$u1 = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/auth/login" -Body @{
  username = "hesphoros"
  password = "hesphoros"
}
$u2 = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/auth/login" -Body @{
  username = "ruansiqi"
  password = "ruansiqi"
}
Assert-True (-not [string]::IsNullOrWhiteSpace($u1.token)) "u1 token should exist"
Assert-True (-not [string]::IsNullOrWhiteSpace($u2.token)) "u2 token should exist"
$h1 = @{ Authorization = "Bearer $($u1.token)" }
$h2 = @{ Authorization = "Bearer $($u2.token)" }

Write-Host "== Step 2: conversation discovery =="
$convList1 = Invoke-ApiJson -Method Get -Url "$BaseUrl/api/v1/conversations" -Headers $h1
Assert-True ($convList1.Count -ge 1) "u1 should have at least 1 conversation"
$convId = [int64]$convList1[0].id
Write-Host "conversationId=$convId"

Write-Host "== Step 3: text roundtrip =="
$t1 = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/messages/send" -Headers $h1 -Body @{
  conversationId = $convId
  msgType = "text"
  contentText = "e2e-text-from-hesphoros"
  clientMsgId = "e2e-u1-text-001"
}
$t2 = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/messages/send" -Headers $h2 -Body @{
  conversationId = $convId
  msgType = "text"
  contentText = "e2e-text-from-ruansiqi"
  clientMsgId = "e2e-u2-text-001"
}
Assert-True ($t1.id -gt 0) "u1 text message id should be > 0"
Assert-True ($t2.id -gt 0) "u2 text message id should be > 0"

Write-Host "== Step 4: prepare image file =="
$tmpDir = Join-Path $PSScriptRoot ".tmp"
if (-not (Test-Path $tmpDir)) {
  New-Item -ItemType Directory -Path $tmpDir | Out-Null
}
$pngPath = Join-Path $tmpDir "e2e_pixel.png"
if (-not (Test-Path $pngPath)) {
  $pngBase64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9oN9bT8AAAAASUVORK5CYII="
  [System.IO.File]::WriteAllBytes($pngPath, [Convert]::FromBase64String($pngBase64))
}
Assert-True (Test-Path $pngPath) "test png file should exist"

Write-Host "== Step 5: upload media and send image/sticker messages =="
$uploadResp = Invoke-RestMethod -SkipCertificateCheck -Method Post -Uri "$BaseUrl/api/v1/media/upload" -Headers $h1 -Form @{
  file = Get-Item $pngPath
}
Assert-True (-not [string]::IsNullOrWhiteSpace($uploadResp.mediaUrl)) "mediaUrl should exist after upload"
$mediaUrl = $uploadResp.mediaUrl
Write-Host "uploaded mediaUrl=$mediaUrl"

$imgMsg = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/messages/send" -Headers $h1 -Body @{
  conversationId = $convId
  msgType = "image"
  mediaUrl = $mediaUrl
  clientMsgId = "e2e-u1-image-001"
}
$stickerMsg = Invoke-ApiJson -Method Post -Url "$BaseUrl/api/v1/messages/send" -Headers $h2 -Body @{
  conversationId = $convId
  msgType = "sticker"
  mediaUrl = $mediaUrl
  clientMsgId = "e2e-u2-sticker-001"
}
Assert-True ($imgMsg.msgType -eq "image") "image msg type mismatch"
Assert-True ($stickerMsg.msgType -eq "sticker") "sticker msg type mismatch"

Write-Host "== Step 6: chat history sync assertions =="
$all = Invoke-ApiJson -Method Get -Url "$BaseUrl/api/v1/messages/sync?conversationId=$convId&afterId=0&limit=200" -Headers $h1
Assert-True ($all.messages.Count -ge 4) "history should contain at least 4 test messages"
$last4 = @($all.messages | Select-Object -Last 4)
Assert-True ($last4[0].msgType -eq "text") "last4[0] should be text"
Assert-True ($last4[1].msgType -eq "text") "last4[1] should be text"
Assert-True ($last4[2].msgType -eq "image") "last4[2] should be image"
Assert-True ($last4[3].msgType -eq "sticker") "last4[3] should be sticker"

$delta = Invoke-ApiJson -Method Get -Url "$BaseUrl/api/v1/messages/sync?conversationId=$convId&afterId=$($last4[1].id)&limit=10" -Headers $h2
Assert-True ($delta.messages.Count -ge 2) "delta after second text should include image+sticker"

Write-Host "== Step 7: protected media download =="
$download = Invoke-WebRequest -SkipCertificateCheck -Headers $h2 -Uri "$BaseUrl$mediaUrl"
Assert-True ($download.StatusCode -eq 200) "media download should be 200"
Assert-True ($download.RawContentLength -gt 0) "media content should not be empty"

Write-Host ""
Write-Host "E2E PASS"
Write-Host "u1=$($u1.user.username), u2=$($u2.user.username)"
Write-Host "convId=$convId, mediaUrl=$mediaUrl"
Write-Host "last4 types = $($last4[0].msgType),$($last4[1].msgType),$($last4[2].msgType),$($last4[3].msgType)"
