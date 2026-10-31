[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM\d+$')]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [ValidateRange(0.001, 0.05)]
    [double]$Duty,

    [string]$OutputDirectory,

    [ValidateRange(1, 10)]
    [int]$Repeat = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $PSScriptRoot '..\measurements'
}

$modes = @('uv_pos', 'uv_neg', 'vw_pos', 'vw_neg', 'wu_pos', 'wu_neg')
$csvHeader = 'sample,mode,duty,sector,u1_raw,v_raw,u2_raw,w_raw,u1_current,v_current,u2_current,w_current'
$sampleCount = 4000
$commandTimeout = [TimeSpan]::FromSeconds(8)
$captureTimeout = [TimeSpan]::FromSeconds(60)
$dutyText = $Duty.ToString('0.000', [Globalization.CultureInfo]::InvariantCulture)
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$runDirectory = Join-Path $OutputDirectory ("current_test_{0}_duty_{1}" -f $timestamp, $dutyText)
$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.NewLine = "`n"
$serial.Encoding = [Text.Encoding]::ASCII
$serial.ReadTimeout = 1000
$serial.WriteTimeout = 1000
$serial.DtrEnable = $false
$serial.RtsEnable = $false

function Write-DeviceCommand {
    param([Parameter(Mandatory = $true)][string]$Command)
    Write-Host "> $Command"
    $script:serial.Write("$Command`r`n")
}

function Read-DeviceLine {
    param([Parameter(Mandatory = $true)][datetime]$Deadline)

    while ([datetime]::UtcNow -lt $Deadline) {
        try {
            return $script:serial.ReadLine().TrimEnd("`r")
        }
        catch [TimeoutException] {
            # Recheck the overall deadline.
        }
    }
    throw 'Timed out waiting for a serial response.'
}

function Wait-ForLine {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Condition,
        [Parameter(Mandatory = $true)][TimeSpan]$Timeout
    )

    $deadline = [datetime]::UtcNow + $Timeout
    while ($true) {
        $line = Read-DeviceLine -Deadline $deadline
        if ($line.Length -gt 0) {
            Write-Host "< $line"
        }
        if ($line -match '^(ADC capture aborted|ADC capture .*failed|ADC capture timed out)') {
            throw $line
        }
        if (& $Condition $line) {
            return $line
        }
    }
}

function Stop-CurrentTest {
    if ($script:serial.IsOpen) {
        try {
            $script:serial.Write("current_test off`r`n")
        }
        catch {
            Write-Warning "Could not send current_test off: $($_.Exception.Message)"
        }
    }
}

New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

try {
    $serial.Open()
    Start-Sleep -Milliseconds 250
    $serial.DiscardInBuffer()

    Write-DeviceCommand 'current_test off'
    Wait-ForLine -Timeout $commandTimeout -Condition {
        param($line)
        $line -match '^(Current test off|PWM stopped)'
    } | Out-Null

    for ($repeatIndex = 1; $repeatIndex -le $Repeat; $repeatIndex++) {
        foreach ($mode in $modes) {
            Write-Host ("Acquiring {0}, repeat {1}/{2}, duty={3}" -f $mode, $repeatIndex, $Repeat, $dutyText)

            Write-DeviceCommand "current_test duty $dutyText"
            Wait-ForLine -Timeout $commandTimeout -Condition {
                param($line)
                $line -match '^Current test:'
            } | Out-Null

            Write-DeviceCommand "current_test $mode"
            $escapedMode = [regex]::Escape($mode)
            Wait-ForLine -Timeout $commandTimeout -Condition {
                param($line)
                $line -match "^Current test: selected=$escapedMode, duty=$dutyText, PWM=on"
            } | Out-Null

            Write-DeviceCommand 'adc'
            Wait-ForLine -Timeout $commandTimeout -Condition {
                param($line)
                $line -eq "ADC capture started: $sampleCount samples"
            } | Out-Null

            Wait-ForLine -Timeout $captureTimeout -Condition {
                param($line)
                $line -eq $csvHeader
            } | Out-Null

            $suffix = if ($Repeat -eq 1) { '' } else { '_{0:D2}' -f $repeatIndex }
            $finalPath = Join-Path $runDirectory ("{0}{1}.csv" -f $mode, $suffix)
            $partialPath = "$finalPath.partial"
            $writer = [IO.StreamWriter]::new($partialPath, $false, [Text.UTF8Encoding]::new($false))
            try {
                $writer.WriteLine($csvHeader)
                $deadline = [datetime]::UtcNow + $captureTimeout
                for ($sample = 0; $sample -lt $sampleCount; $sample++) {
                    $line = Read-DeviceLine -Deadline $deadline
                    $columns = $line.Split(',')
                    if (($columns.Count -ne 12) -or
                        ($columns[0] -ne $sample.ToString([Globalization.CultureInfo]::InvariantCulture)) -or
                        ($columns[1] -ne $mode) -or
                        ($columns[2] -ne $dutyText)) {
                        throw "Unexpected CSV row at sample ${sample}: $line"
                    }
                    $writer.WriteLine($line)
                }
            }
            finally {
                $writer.Dispose()
            }

            Move-Item -LiteralPath $partialPath -Destination $finalPath
            Write-Host "Saved: $finalPath"
        }
    }

    Write-Host "Completed all captures: $runDirectory"
}
finally {
    Stop-CurrentTest
    if ($serial.IsOpen) {
        Start-Sleep -Milliseconds 100
        $serial.Close()
    }
    $serial.Dispose()
}
