// SPDX-License-Identifier: GPL-3.0-or-later
// pkg-oracle build <source-folder> <output-folder> <content-id>
// pkg-oracle extract <package> <output-folder> [passcode]
using LibProsperoPkg;
using LibProsperoPkg.PKG;

switch (args.Length > 0 ? args[0] : "")
{
    case "build":
    {
        var result = ProsperoPackageBuilder.Build(new ProsperoBuildOptions
        {
            Mode = ProsperoPackageMode.Application,
            OutputFormat = ProsperoOutputFormat.DebugImage,
            SourceFolder = args[1],
            OutputFolder = args[2],
            ContentId = args[3],
            TitleId = args[3].Substring(7, 9),
            Title = "Alembic test",
        }, _ => { });
        Console.WriteLine(result.OutputPath);
        return 0;
    }
    case "extract":
        ProsperoPackageExtractor.Extract(args[1], args[2], args.Length > 3 ? args[3] : new string('0', 32));
        return 0;
    default:
        Console.Error.WriteLine("usage: pkg-oracle build|extract ...");
        return 2;
}
