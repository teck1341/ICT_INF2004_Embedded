param([Parameter(Mandatory=$true)][string]$InputDocx,
      [Parameter(Mandatory=$true)][string]$OutputPdf)
$ErrorActionPreference = 'Stop'
if (Get-Process WINWORD -ErrorAction SilentlyContinue) {
    throw 'An existing Word process is active; refusing to affect it.'
}
$inputFile = (Resolve-Path -LiteralPath $InputDocx).Path
$outputFile = [IO.Path]::GetFullPath($OutputPdf)
New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($outputFile)) | Out-Null
$word = $null
$document = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $word.AutomationSecurity = 3
    $document = $word.Documents.Open($inputFile, $false, $true, $false)
    $document.Repaginate()
    $pages = $document.ComputeStatistics(2)
    $document.ExportAsFixedFormat($outputFile, 17)
    [pscustomobject]@{input=$inputFile; pdf=$outputFile; pages=$pages; renderer='Microsoft Word COM'; readOnly=$true} | ConvertTo-Json
} finally {
    $discard = 0
    if ($word) {
        # After ExportAsFixedFormat, Word can disconnect the original Document
        # proxy of a report that has content controls and PAGEREF fields (the
        # document itself stays open and intact, 25 September 2026), so close
        # every document through the Documents collection instead.
        while ($word.Documents.Count -gt 0) { $word.Documents.Item(1).Close([ref]$discard) }
        $word.Quit([ref]$discard)
    }
    if ($document) { try { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($document) } catch {} }
    if ($word) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($word) }
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
}
