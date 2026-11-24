namespace Gkms.Dlss5;

public sealed class Options
{
    public bool EnableNeuralRendering { get; set; }
    public string RuntimeDirectory { get; set; } = "";
    public float Intensity { get; set; } = 1;
    public float ToneStrength { get; set; } = .25f;
    public float StructureStrength { get; set; } = .5f;
    public float SkinStrength { get; set; }
    public bool AutoMask { get; set; } = true;
    public bool ComparisonSplit { get; set; }
    public float MotionScaleX { get; set; } = -1;
    public float MotionScaleY { get; set; } = -1;
    public int Preset { get; set; }
    public float ModelScale { get; set; } = .5f;
    public float WhitePoint { get; set; } = 1;
    public float ExposureEV { get; set; }
    public float TransferStrength { get; set; } = 1;
    public float ColorStrength { get; set; } = .5f;
    public float MaxRatio { get; set; } = 2;
    public int Codec { get; set; }
    public bool AutoExposure { get; set; }
    public int InputEncoding { get; set; }
    public int Style { get; set; }
    public bool Capture { get; set; }
    public bool FlipY { get; set; } = true;
    public int DownFilter { get; set; } = 3;
    public int TransferMode { get; set; } = 1;
    public bool ApplyModel { get; set; } = true;
    public bool HoldFrame { get; set; }
    public int CompareMode { get; set; } = 2;
    public bool CompareSwap { get; set; }
    public bool CompareLabels { get; set; }
    public int DebugView { get; set; }
    public float ComparePosition { get; set; } = .5f;
    public float CompareZoom { get; set; } = 1;
    public float DebugGain { get; set; } = 4;
    public bool UseDepth { get; set; } = true;
    public bool DepthInverted { get; set; } = true;
    public void Validate()
    {
        foreach (var value in new[] { Intensity, ToneStrength, StructureStrength })
            if (!float.IsFinite(value) || value < 0 || value > 2)
                throw new ArgumentOutOfRangeException(nameof(Intensity), "增强强度必须在 0–2 之间。");
        if (!float.IsFinite(MotionScaleX) || !float.IsFinite(MotionScaleY) || Math.Abs(MotionScaleX) > 4 || Math.Abs(MotionScaleY) > 4)
            throw new ArgumentOutOfRangeException(nameof(MotionScaleX), "运动矢量倍率必须在 -4–4 之间。");
        if (Preset < 0 || Preset > 3) throw new ArgumentOutOfRangeException(nameof(Preset));
        static void Range(float value, float min, float max, string name)
        { if (!float.IsFinite(value) || value < min || value > max) throw new ArgumentOutOfRangeException(name); }
        Range(ModelScale, .25f, 2, nameof(ModelScale)); Range(WhitePoint, .01f, 4096, nameof(WhitePoint));
        Range(ExposureEV, -4, 4, nameof(ExposureEV)); Range(TransferStrength, 0, 2, nameof(TransferStrength));
        Range(SkinStrength, -1, 2, nameof(SkinStrength));
        Range(ColorStrength, 0, 4, nameof(ColorStrength)); Range(MaxRatio, 1, 8, nameof(MaxRatio));
        Range(ComparePosition,0,1,nameof(ComparePosition));Range(CompareZoom,1,2,nameof(CompareZoom));Range(DebugGain,1,32,nameof(DebugGain));
        if(DownFilter<0||DownFilter>3||TransferMode<0||TransferMode>1||CompareMode<1||CompareMode>2||DebugView<0||DebugView>3)throw new ArgumentException("滤镜或对比模式超出范围。");
        if (Codec < 0 || Codec > 4 || InputEncoding < 0 || InputEncoding > 1 || Style < 0 || Style > 3)
            throw new ArgumentException("色彩模式或模型风格超出范围。");
        if (EnableNeuralRendering && (string.IsNullOrWhiteSpace(RuntimeDirectory) || !Path.IsPathFullyQualified(RuntimeDirectory)))
            throw new ArgumentException("启用神经渲染前，请设置包含 nvngx_dlssnr.dll 的绝对目录。");
        if (EnableNeuralRendering && !File.Exists(Path.Combine(RuntimeDirectory, "nvngx_dlssnr.dll")))
            throw new FileNotFoundException("找不到 DLSS 5 NR 运行库。", Path.Combine(RuntimeDirectory, "nvngx_dlssnr.dll"));
    }
}
