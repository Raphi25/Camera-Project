"""Quantify motion-blur captures from a TinyUSB download."""
import argparse
import csv
import re
from pathlib import Path
import numpy as np
from PIL import Image

PROFILE = re.compile(r"(?:run_\d{5}_)?\d{6}_(?P<profile>[A-Za-z0-9]+_a\d+)\.jpg$")

def metrics(path):
    """Return Laplacian variance and Tenengrad energy for the image centre."""
    with Image.open(path) as image:
        gray = np.asarray(image.convert("L"), dtype=np.float32)
    h, w = gray.shape
    gray = gray[int(.05*h):int(.95*h), int(.05*w):int(.95*w)]
    lap = (-4*gray + np.roll(gray, 1, 0) + np.roll(gray, -1, 0) +
           np.roll(gray, 1, 1) + np.roll(gray, -1, 1))[1:-1, 1:-1]
    gx, gy = np.diff(gray, axis=1), np.diff(gray, axis=0)
    return float(np.var(lap)), float(np.mean(gx*gx) + np.mean(gy*gy))

def metadata(path):
    """Read the firmware CSV sidecar, returning an empty record if absent."""
    try:
        with Path(str(path)+".csv").open(newline="") as f: return next(csv.DictReader(f))
    except (OSError, StopIteration, csv.Error): return {}

def main():
    p=argparse.ArgumentParser(); p.add_argument("folder",type=Path,nargs="?", help="TinyUSB download folder (defaults to newest download)")
    p.add_argument("--csv",type=Path); p.add_argument("--plot",type=Path); a=p.parse_args()
    if a.folder is None:
        candidates=sorted((Path(__file__).resolve().parent/"retrieved_images").glob("download_*") , key=lambda x: x.stat().st_mtime, reverse=True)
        if not candidates: p.error("No download folder found under retrieved_images")
        a.folder=candidates[0]
        print(f"Using newest download: {a.folder}")
    images=sorted(a.folder.rglob("*.jpg")); rows=[]; errors=[]
    if not images: p.error(f"No JPEG files found below {a.folder}")
    for image in images:
        match=PROFILE.search(image.name)
        if not match: continue
        m=metadata(image)
        try:
            lap,ten=metrics(image)
        except (OSError, ValueError) as exc:
            errors.append((image, str(exc))); continue
        rows.append({"file":str(image.relative_to(a.folder)),"profile":match["profile"],
          "amplitude_deg":m.get("amplitude_deg",""),"velocity_deg_s":m.get("velocity_deg_s",""),
          "expected_blur_px":m.get("expected_blur_px",""),"laplacian_variance":f"{lap:.4f}","tenengrad":f"{ten:.4f}"})
    if errors:
        print(f"Unreadable JPEGs: {len(errors)}")
        for image, error in errors[:20]: print(f"  {image.name}: {error}")
    if not rows: p.error("No readable JPEG files found; fix image capture/transfer before measuring blur")
    groups={}
    for r in rows: groups.setdefault(r["profile"],[]).append(r)
    baseline_values=[float(r["laplacian_variance"]) for r in groups.get("baseline_a0",[])]
    baseline_ref=float(np.median(baseline_values)) if baseline_values else 0.0
    for g in groups.values():
        ref=max(float(r["laplacian_variance"]) for r in g)
        for r in g:
            ratio=float(r["laplacian_variance"])/ref if ref else 0
            r["sharpness_relative"]=f"{ratio:.4f}"; r["blur_relative"]=f"{1-ratio:.4f}"
            baseline_ratio=(float(r["laplacian_variance"])/baseline_ref) if baseline_ref else 0
            r["sharpness_vs_baseline"]=f"{baseline_ratio:.4f}"
            r["blur_vs_baseline"]=f"{1-baseline_ratio:.4f}"
    out=a.csv or a.folder/"blur_analysis.csv"
    with out.open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    print(f"Analyzed {len(rows)} images\nSummary: {out.resolve()}")
    for profile,g in sorted(groups.items()):
        expected=g[0]["expected_blur_px"] or "n/a"
        raw=np.mean([float(r["laplacian_variance"]) for r in g])
        normalized=(1.0 if profile == "baseline_a0" else
                    np.mean([float(r["sharpness_vs_baseline"]) for r in g])) if baseline_ref else float("nan")
        print(f"{profile:12s} expected {expected:>7s} px, mean Laplacian {raw:>7.1f}, sharpness vs baseline {normalized:.3f}")
    plot_path = a.plot or a.folder/"blur_analysis.png"
    import matplotlib.pyplot as plt
    figure, axes = plt.subplots(1, 2, figsize=(13, 5))
    labels, sharpness, expected = [], [], []
    for profile, group in sorted(groups.items()):
        labels.append(profile)
        sharpness.append(float(np.mean([float(r["laplacian_variance"]) for r in group])))
        expected.append(float(group[0]["expected_blur_px"] or 0))
    x=np.arange(len(labels))
    axes[0].bar(x, sharpness, color="tab:blue", alpha=.8, label="Measured sharpness (Laplacian)")
    axes[0].set_xticks(x, labels, rotation=45, ha="right")
    axes[0].set_ylabel("Mean Laplacian variance (sharpness)", color="tab:blue")
    axes[0].tick_params(axis="y", labelcolor="tab:blue")
    red_axis=axes[0].twinx()
    red_axis.plot(x, expected, color="red", marker="o", linewidth=2, label="Expected blur")
    red_axis.set_ylabel("Expected blur during exposure (pixels)", color="red")
    red_axis.tick_params(axis="y", labelcolor="red")
    axes[0].set_title("Expected blur and measured sharpness")
    axes[0].grid(axis="y", alpha=.25)
    lines, names = axes[0].get_legend_handles_labels(); lines2, names2 = red_axis.get_legend_handles_labels()
    axes[0].legend(lines+lines2, names+names2, loc="upper right")
    labels, scores = [], []
    for profile, group in sorted(groups.items()):
        labels.append(profile)
        scores.append(0.0 if profile == "baseline_a0" else
                      100*np.mean([float(r["blur_vs_baseline"]) for r in group]))
    axes[1].bar(labels, scores)
    axes[1].set_ylabel("Blur relative to stationary baseline (%)")
    axes[1].set_title("Baseline-normalized blur")
    axes[1].tick_params(axis="x", rotation=55)
    axes[1].grid(axis="y", alpha=.25)
    figure.tight_layout(); figure.savefig(plot_path, dpi=160); plt.close(figure)
    print(f"Plot: {plot_path.resolve()}")

if __name__ == "__main__": main()
