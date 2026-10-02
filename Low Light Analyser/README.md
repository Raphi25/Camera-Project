# OV5647 low-light image analysis

Run the full pipeline with the dataset's current location and the default centered 50% crop:

```powershell
python analyze_low_light.py
```

Use a different normalized crop (`x y width height`) or output folder:

```powershell
python analyze_low_light.py --crop 0.35 0.35 0.30 0.30 --output my_results
```

The script expects one folder per lux level, named like `35LUX`. Each folder can contain any positive number of images; different lux folders may have different image counts. It creates per-image and per-lux CSV files, a formatted Excel workbook with charts, a PNG quality plot, average images, and repeat-standard-deviation maps. Between-repeat standard deviation is unavailable for a folder containing only one image.

Noise is estimated from the high-frequency residual after Gaussian smoothing. This reduces contamination by broad scene gradients, though textured content can still increase the estimate. Repeat variability is pixelwise standard deviation across unregistered repeats, so the camera and scene should remain fixed.

## Separate grayscale and colour-chart analysis

The chart analyzer is a practical, separate-chart, ISO 19093-inspired workflow. It does not claim full ISO 19093 conformity.

Arrange captures as follows:

```text
chart_dataset/
  grayscale/35LUX/image1.jpg ... image5.jpg
  grayscale/75LUX/image1.jpg ... image5.jpg
  colorchecker/35LUX/image1.jpg ... image5.jpg
  colorchecker/75LUX/image1.jpg ... image5.jpg
```

Edit `chart_config.json` so each chart's four normalized corner coordinates describe, in order, its top-left, top-right, bottom-right and bottom-left corners. Coordinates range from 0 to 1 relative to image width and height. The supplied defaults are placeholders. Use the generated diagnostic overlays to confirm every numbered ROI lies inside the intended patch.

Run without calibrated references:

```powershell
python analyze_test_charts.py "C:\path\to\chart_dataset"
```

Run with calibrated patch data:

```powershell
python analyze_test_charts.py "C:\path\to\chart_dataset" `
  --gray-reference grayscale_reference.csv `
  --color-reference color_reference.csv
```

Copy and complete `grayscale_reference_template.csv` with measured relative luminance or reflectance. Copy and complete `color_reference_template.csv` with measured CIELAB reference values. OECF exposure-proxy columns and ΔE76 are only produced when those references are supplied.

Outputs include per-image patch measurements, lux/patch summaries, grayscale OECF/noise/SNR plots, colour chroma/noise/SNR/ΔE plots, diagnostic patch overlays, CSV files and a formatted Excel workbook.
