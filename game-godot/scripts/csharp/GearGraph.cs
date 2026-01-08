using Godot;
using System.Collections.Generic;
using System.Linq;

namespace ApexSim;

public partial class GearGraph : Control
{
    private List<float> _ratios = new();
    private float _finalDrive = 1.0f;

    public void SetGearRatios(List<float> ratios, float finalDrive)
    {
        _ratios = ratios ?? new List<float>();
        _finalDrive = finalDrive <= 0 ? 1.0f : finalDrive;
        QueueRedraw();
    }

    public override void _Draw()
    {
        base._Draw();

        var rect = new Rect2(Vector2.Zero, GetRect().Size);
        var w = rect.Size.X;
        var h = rect.Size.Y;

        // background
        DrawRect(new Rect2(0, 0, w, h), new Color(18/255.0f, 18/255.0f, 18/255.0f, 1.0f));

        if (_ratios == null || _ratios.Count == 0)
        {
            // gracefully do nothing if no data
            return;
        }

        // Normalize by max effective ratio
        var effective = _ratios.Select(r => r * _finalDrive).Where(r => r > 0).ToList();
        if (effective.Count == 0) return;
        var max = effective.Max();

        int n = effective.Count;
        float paddingX = 40;
        float paddingY = 30;
        float maxGraphWidth = 400; // Maximum width for the graph lines
        float availW = Mathf.Min(w - paddingX * 2, maxGraphWidth);
        float availH = h - paddingY * 2;

        // Draw stacked lines visualization
        // Each gear is represented as a horizontal line segment at a height proportional to its ratio
        // The lines are stacked from top (highest ratio) to bottom (lowest ratio)

        var sortedIndices = Enumerable.Range(0, effective.Count)
            .OrderByDescending(i => effective[i])
            .ToList();

        // Draw grid lines
        var gridColor = new Color(0.3f, 0.3f, 0.3f, 0.4f);
        for (int i = 0; i <= 4; i++)
        {
            float y = paddingY + (availH / 4) * i;
            DrawLine(new Vector2(paddingX, y), new Vector2(paddingX + availW, y), gridColor, 1);
        }

        // Define colors for different gears
        var colors = new Color[]
        {
            new Color(1.0f, 0.3f, 0.3f, 1.0f), // Reverse - red
            new Color(0.3f, 0.8f, 1.0f, 1.0f), // 1st - light blue
            new Color(0.3f, 1.0f, 0.5f, 1.0f), // 2nd - green
            new Color(1.0f, 1.0f, 0.3f, 1.0f), // 3rd - yellow
            new Color(1.0f, 0.6f, 0.2f, 1.0f), // 4th - orange
            new Color(0.8f, 0.3f, 1.0f, 1.0f), // 5th - purple
            new Color(1.0f, 0.4f, 0.7f, 1.0f), // 6th - pink
            new Color(0.5f, 1.0f, 1.0f, 1.0f), // 7th - cyan
            new Color(1.0f, 0.8f, 0.5f, 1.0f), // 8th - light orange
        };

        // Draw lines for each gear
        float lineThickness = 8f;
        float spacing = availH / (n + 1);

        for (int i = 0; i < n; i++)
        {
            int gearIndex = i;
            float val = effective[gearIndex];
            float norm = val / max;
            float lineWidth = norm * availW;

            float y = paddingY + spacing * (i + 1);
            float startX = paddingX;
            float endX = paddingX + lineWidth;

            var color = colors[gearIndex % colors.Length];

            // Draw the line
            DrawLine(new Vector2(startX, y), new Vector2(endX, y), color, lineThickness);

            // Draw gear label at the start
            var gearName = gearIndex == 0 ? "R" : gearIndex.ToString();
            DrawString(ThemeDB.FallbackFont, new Vector2(startX - 25, y + 5), gearName, HorizontalAlignment.Left, -1, 14, new Color(0.9f, 0.9f, 0.9f));

            // Draw ratio value at the end
            var ratioText = $"{val:F2}";
            DrawString(ThemeDB.FallbackFont, new Vector2(endX + 8, y + 5), ratioText, HorizontalAlignment.Left, -1, 12, new Color(0.7f, 0.7f, 0.7f));
        }

        // Draw title
        DrawString(ThemeDB.FallbackFont, new Vector2(paddingX, paddingY - 10), "Gear Ratios (x Final Drive)", HorizontalAlignment.Left, -1, 14, new Color(0.9f, 0.9f, 0.9f));
    }
}
