param(
    [string]$Source = (Join-Path $PSScriptRoot '..\..\Resources\AppIcon-1024.png'),
    [string]$Destination = (Join-Path $PSScriptRoot '..\src\AIVideoCutty.ico')
)

Add-Type -AssemblyName System.Drawing
$original = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source))
$images = @()
try {
    foreach ($size in @(256, 48, 32, 16)) {
        $bitmap = [System.Drawing.Bitmap]::new($size, $size)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $stream = [System.IO.MemoryStream]::new()
        try {
            $graphics.Clear([System.Drawing.Color]::Transparent)
            $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $graphics.DrawImage($original, 0, 0, $size, $size)
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            $images += [pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
        } finally {
            $stream.Dispose()
            $graphics.Dispose()
            $bitmap.Dispose()
        }
    }
} finally {
    $original.Dispose()
}

$target = [System.IO.File]::Open($Destination, [System.IO.FileMode]::Create)
$writer = [System.IO.BinaryWriter]::new($target)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
        $writer.Write([byte]($image.Size % 256))
        $writer.Write([byte]($image.Size % 256))
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$image.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $image.Bytes.Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image.Bytes) }
} finally {
    $writer.Dispose()
}
