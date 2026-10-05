# Generates independent UDF fixtures through Windows IMAPI, without disc writes.
# Run from the repository root, then pulse_udf_listing_test --image <output>.
$ErrorActionPreference = 'Stop'
$fixtureRoot = Join-Path (Get-Location) 'bench_data\udf_imapi_source'
[void](New-Item -ItemType Directory -Force -Path (Join-Path $fixtureRoot 'folder'))
$unicodeName = ([string][char]0x9605) + [char]0x8bfb + '.txt'
[IO.File]::WriteAllBytes((Join-Path $fixtureRoot $unicodeName), (New-Object byte[] 12345))
[IO.File]::WriteAllBytes((Join-Path $fixtureRoot 'folder\nested.txt'), (New-Object byte[] 54321))
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public static class UdfFixtureStream {
    public static void Save(object source, string path) {
        IStream stream = (IStream)source;
        IntPtr count = Marshal.AllocHGlobal(4);
        try {
            stream.Seek(0, 0, IntPtr.Zero);
            using (var file = new FileStream(path, FileMode.Create, FileAccess.Write)) {
                byte[] buffer = new byte[65536];
                for (;;) {
                    stream.Read(buffer, buffer.Length, count);
                    int n = Marshal.ReadInt32(count);
                    if (n == 0) break;
                    file.Write(buffer, 0, n);
                }
            }
        } finally { Marshal.FreeHGlobal(count); }
    }
}
'@
foreach ($revision in @(0x102, 0x150, 0x200, 0x201, 0x250)) {
    $image = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
    try {
        $image.FileSystemsToCreate = 4
        $image.UDFRevision = $revision
        $image.VolumeName = 'PULSE_UDF_TEST'
        $image.Root.AddTree($fixtureRoot, $false)
        $result = $image.CreateResultImage()
        $output = Join-Path (Get-Location) ('bench_data\udf_imapi_{0:x}.iso' -f $revision)
        [UdfFixtureStream]::Save($result.ImageStream, $output)
        Write-Output $output
    } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($image) }
}
