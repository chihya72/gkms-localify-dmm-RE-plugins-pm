namespace Gkms.Dlss5;

// Never pair motion from another camera/frame with a final colour buffer.
internal sealed class FrameSelection<T> where T : class
{
    private readonly Dictionary<nint, T> frames = new();
    private int frame = -1;
    public void Record(int number, nint camera, T value)
    {
        Advance(number);
        frames[camera] = value;
    }
    public bool TryTake(int number, nint camera, out T? value)
    {
        Advance(number);
        return frames.Remove(camera, out value);
    }
    private void Advance(int number)
    {
        if (frame == number) return;
        frames.Clear(); frame = number;
    }
    public void Clear() { frames.Clear(); frame = -1; }
}
