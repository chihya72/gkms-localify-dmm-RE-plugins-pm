using Gkms.Dlss5;

static void Require(bool value, string message) { if (!value) throw new Exception(message); }
static void Reject(Action action)
{
    try { action(); } catch (ArgumentException) { return; }
    throw new Exception("Invalid configuration was accepted");
}
var selection = new FrameSelection<object>();
var first = new object(); var second = new object();
selection.Record(1, 101, first); selection.Record(1, 202, second);
Require(selection.TryTake(1, 101, out var selected) && ReferenceEquals(selected, first), "Wrong camera selected");
Require(!selection.TryTake(1, 101, out _), "Same frame processed twice");
Require(!selection.TryTake(2, 202, out _), "Stale motion reused in next frame");
selection.Record(3, 101, first); selection.Clear();
Require(!selection.TryTake(3, 101, out _), "Dispose retained motion texture");
new Options().Validate();
Reject(() => new Options { Intensity = float.NaN }.Validate());
new Options { Intensity=2, ToneStrength=2, StructureStrength=2, SkinStrength=-1, TransferStrength=2 }.Validate();
Reject(() => new Options { ToneStrength = 2.01f }.Validate());
Reject(() => new Options { SkinStrength = -1.01f }.Validate());
Reject(() => new Options { TransferStrength = 2.01f }.Validate());
Reject(() => new Options { MotionScaleX = float.PositiveInfinity }.Validate());
Reject(() => new Options { ModelScale = 0 }.Validate());
Reject(() => new Options { WhitePoint = 0 }.Validate());
Reject(() => new Options { ExposureEV = float.NaN }.Validate());
new Options { ModelScale=2,ColorStrength=4,MaxRatio=8,HoldFrame=true,CompareMode=1,CompareZoom=2,DebugView=3 }.Validate();
Reject(() => new Options { ColorStrength = 4.01f }.Validate());
Reject(() => new Options { ModelScale = 2.01f }.Validate());
Reject(() => new Options { ComparePosition = float.NaN }.Validate());
Reject(() => new Options { CompareMode = 3 }.Validate());
Reject(() => new Options { DebugView = 4 }.Validate());
Reject(() => new Options { MaxRatio = .5f }.Validate());
Reject(() => new Options { Codec = 5 }.Validate());
Reject(() => new Options { EnableNeuralRendering = true }.Validate());
Reject(() => new Options { EnableNeuralRendering = true, RuntimeDirectory = "relative" }.Validate());
Console.WriteLine("PASS: same-frame/same-camera selection, one-time consumption, disposal, invalid NR settings");
