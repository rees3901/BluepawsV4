param(
    [ValidateSet('Status', 'Configure', 'Send', 'Listen')]
    [string]$Action = 'Status',
    [string]$Port = 'COM13',
    [string]$PayloadHex = '',
    [ValidateRange(1, 3)]
    [int]$Count = 1,
    [ValidateRange(1, 60)]
    [int]$ListenSeconds = 10
)

$ErrorActionPreference = 'Stop'
$expected = [ordered]@{
    'AT+NWM=?' = 'AT+NWM=0'
    'AT+P2P=?' = 'AT+P2P=869500000:10:0:1:8:5'
    'AT+SYNCWORD=?' = 'AT+SYNCWORD=1424'
    'AT+IQINVER=?' = 'AT+IQINVER=0'
    'AT+ENCRY=?' = 'AT+ENCRY=0'
    'AT+FIXLENGTHPAYLOAD=?' = 'AT+FIXLENGTHPAYLOAD=0'
    'AT+CAD=?' = 'AT+CAD=1'
}

function Read-Until([System.IO.Ports.SerialPort]$Serial, [string]$Pattern, [int]$TimeoutMs) {
    $buffer = ''
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.ElapsedMilliseconds -lt $TimeoutMs) {
        $buffer += $Serial.ReadExisting()
        if ($buffer -match $Pattern) { return $buffer.Trim() }
        Start-Sleep -Milliseconds 50
    }
    return $buffer.Trim()
}

function Invoke-AT([System.IO.Ports.SerialPort]$Serial, [string]$Command, [int]$TimeoutMs = 2000) {
    $Serial.Write($Command + "`r`n")
    $reply = Read-Until $Serial '(?m)^(OK|AT_[A-Z_]+)\s*$' $TimeoutMs
    Write-Host ($Command + ' -> ' + $(if ($reply) { $reply -replace "`r?`n", ' | ' } else { '<no response>' }))
    if ($reply -notmatch '(?m)^OK\s*$') { throw "AT command failed: $Command" }
    return $reply
}

function New-TestPayload {
    # TLV v1.2: 32-byte header, zero TLVs, zeroed eight-byte auth tag.
    # Source FFFD is a chosen diagnostic identity; destination 0000 is cloud,
    # so this cannot be mistaken for a command addressed to a real collar.
    $packet = [byte[]]::new(40)
    $packet[0] = 2
    [Array]::Copy([BitConverter]::GetBytes([uint16]0xFFFD), 0, $packet, 1, 2)
    [Array]::Copy([BitConverter]::GetBytes([uint16](Get-Random -Minimum 1 -Maximum 65535)), 0, $packet, 5, 2)
    [Array]::Copy([BitConverter]::GetBytes([uint32][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()), 0, $packet, 7, 4)
    $packet[11] = 0x40 # DEBUG profile, HOME status
    $packet[26] = 0xFF # unknown fix age
    $packet[27] = 0xFF
    $packet[28] = 0xFF # unknown satellite count
    return [BitConverter]::ToString($packet).Replace('-', '')
}

$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$serial.WriteTimeout = 1500
try {
    $serial.Open()
    $null = Invoke-AT $serial 'AT'

    if ($Action -eq 'Configure') {
        $mode = Invoke-AT $serial 'AT+NWM=?'
        if ($mode -notmatch 'AT\+NWM=0') {
            $null = Invoke-AT $serial 'AT+NWM=0' 4000
            Start-Sleep -Seconds 2
            $serial.DiscardInBuffer()
        }
        foreach ($command in @(
            'AT+P2P=869500000:10:0:1:8:5',
            'AT+SYNCWORD=1424',
            'AT+IQINVER=0',
            'AT+ENCRY=0',
            'AT+FIXLENGTHPAYLOAD=0',
            'AT+CAD=1'
        )) { $null = Invoke-AT $serial $command }
    }

    if ($Action -in @('Configure', 'Status', 'Send', 'Listen')) {
        foreach ($command in $expected.Keys) {
            $reply = Invoke-AT $serial $command
            if ($reply -notmatch [regex]::Escape($expected[$command])) {
                throw "Unexpected setting for $command; expected $($expected[$command])"
            }
        }
    }

    if ($Action -eq 'Send') {
        for ($i = 1; $i -le $Count; $i++) {
            $testHex = if ($PayloadHex) { $PayloadHex.ToUpperInvariant() } else { New-TestPayload }
            if ($testHex -notmatch '^(?:[0-9A-F]{2}){1,64}$') { throw 'PayloadHex must be 1 to 64 bytes of hexadecimal data.' }
            Write-Host "Payload: $testHex"
            $command = 'AT+PSEND=' + $testHex
            $serial.Write($command + "`r`n")
            $reply = Read-Until $serial '\+EVT:TXP2P DONE|AT_[A-Z_]+|\+EVT:TXP2P ERROR' 15000
            Write-Output ("TX $i/$Count -> " + $(if ($reply) { $reply -replace "`r?`n", ' | ' } else { '<no response>' }))
            if ($reply -notmatch '\+EVT:TXP2P DONE') { throw 'Transmit did not complete.' }
            if ($i -lt $Count) { Start-Sleep -Seconds 3 }
        }
    }

    if ($Action -eq 'Listen') {
        $null = Invoke-AT $serial ('AT+PRECV=' + ($ListenSeconds * 1000))
        $reply = Read-Until $serial '\+EVT:RXP2P|\+EVT:RX TIMEOUT' (($ListenSeconds + 2) * 1000)
        Write-Output ('RX -> ' + $(if ($reply) { $reply -replace "`r?`n", ' | ' } else { '<no packet during window>' }))
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
