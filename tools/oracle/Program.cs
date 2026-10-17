// SPDX-License-Identifier: GPL-3.0-or-later
// Oracle commands:
//   exfat <dir> <out> [clusterSize]   PS5PKGTool's exFAT image builder
//   pfsc-encode <in> <out>            PS5PKGTool's PFSC encoder
//   pfsc-decode <in> <out>            PS5PKGTool's PFSC decoder (padded length)
//   ffpfsc-dir <dir> <out> <unixtime> PS5PKGTool's FFPFSC builder for a dump folder
//   ffpfsc-image <in> <out> <unixtime> PS5PKGTool's FFPFSC builder for an image file
//   exfat-extract <image> <out-dir>   PS5PKGTool's exFAT reader: extract every file
//   ffpfsc-verify <in>                PS5PKGTool's FFPFSC verifier: SHA-256 of the decoded payload
//   ufs2-extract <image> <out-dir>    PS5PKGTool's UFS2 reader: extract every file
//   lower                             dump string.ToLowerInvariant() as "from to" hex pairs
using PS5PKGTool.Ffpfsc;

switch (args.Length > 0 ? args[0] : "")
{
    case "exfat":
        await ExfatImage.WriteDirectoryAsync(args[1], args[2],
            new ExfatBuildOptions { ClusterSize = args.Length > 3 ? int.Parse(args[3]) : null });
        return 0;
    case "pfsc-encode":
    {
        using var input = File.OpenRead(args[1]);
        using var output = File.Create(args[2]);
        var r = await PfscCodec.EncodeAsync(input, input.Length, output);
        Console.WriteLine($"blocks={r.BlockCount} compressed={r.CompressedBlockCount} stored={r.StoredLength}");
        return 0;
    }
    case "pfsc-decode":
    {
        using var input = File.OpenRead(args[1]);
        using var output = File.Create(args[2]);
        await PfscCodec.DecodeAsync(input, output);
        return 0;
    }
    case "ffpfsc-dir":
    {
        var r = await FfpfscImage.CreateFromDirectoryAsync(args[1], args[2],
            new FfpfscBuildOptions { BuildTimestampUnixSeconds = long.Parse(args[3]) });
        Console.WriteLine($"inner={r.InnerFileName}");
        return 0;
    }
    case "ffpfsc-image":
    {
        var r = await FfpfscImage.CreateFromImageAsync(args[1], args[2],
            new FfpfscBuildOptions { BuildTimestampUnixSeconds = long.Parse(args[3]) });
        Console.WriteLine($"inner={r.InnerFileName}");
        return 0;
    }
    case "exfat-extract":
        await ExfatImage.ExtractDirectoryAsync(args[1], args[2]);
        return 0;
    case "ffpfsc-verify":
    {
        var r = await FfpfscImage.VerifyAsync(args[1]);
        Console.WriteLine(r.DecodedSha256!.ToLowerInvariant());
        return 0;
    }
    case "ufs2-extract":
        await UFS2Tool.Ufs2Operations.ExtractAsync(args[1], args[2]);
        return 0;
    case "lower":
        for (int cp = 0; cp <= 0x10FFFF; cp++)
        {
            if (cp is >= 0xD800 and <= 0xDFFF) continue;
            string s = char.ConvertFromUtf32(cp);
            string l = s.ToLowerInvariant();
            if (l == s) continue;
            if (l.Length != s.Length) throw new InvalidOperationException($"length change at {cp:X}");
            Console.WriteLine($"{cp:X} {char.ConvertToUtf32(l, 0):X}");
        }
        return 0;
    default:
        Console.Error.WriteLine("usage: oracle exfat|pfsc-encode|pfsc-decode|ffpfsc-dir|ffpfsc-image|lower ...");
        return 2;
}
