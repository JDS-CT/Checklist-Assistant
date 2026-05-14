import argparse
import re
from pathlib import Path


STATUS_MAP = {
    "Provided": "\u2705",  # ✅
    "Partially Provided": "\u2714",  # ✔
    "Not Provided": "\u274c",  # ❌
    "Not Applicable": "\u274c",  # ❌
}


FEATURE_RE = re.compile(r"^# Feature\s*(\d+)\s*[–-]?\s*(.*)$")
VENDOR_RE = re.compile(r"^##\s+(.+?)\s*$")
STATUS_RE = re.compile(r"^\*\*(Provided|Partially Provided|Not Provided|Not Applicable)\b")


def parse_features(lines):
    features = {}
    vendors_in_order = []
    current_feature = None
    current_vendor = None

    for raw_line in lines:
        line = raw_line.rstrip("\n")
        feature_match = FEATURE_RE.match(line)
        if feature_match:
            number = int(feature_match.group(1))
            title = feature_match.group(2).strip()
            features[number] = {
                "title": title,
                "vendors": {},
            }
            current_feature = number
            current_vendor = None
            continue

        if current_feature is None:
            continue

        vendor_match = VENDOR_RE.match(line)
        if vendor_match:
            current_vendor = vendor_match.group(1).strip()
            if current_vendor not in vendors_in_order:
                vendors_in_order.append(current_vendor)
            continue

        if current_vendor is None:
            continue

        vendors = features[current_feature]["vendors"]
        if current_vendor in vendors:
            continue

        status_match = STATUS_RE.match(line.strip())
        if status_match:
            status = status_match.group(1)
            vendors[current_vendor] = STATUS_MAP.get(status, "")

    return features, vendors_in_order


def format_table(features, vendors):
    header = ["Feature"] + vendors
    lines = []
    lines.append("| " + " | ".join(header) + " |")
    lines.append("| " + " | ".join("---" for _ in header) + " |")

    for feature_number in range(1, 23):
        feature = features.get(feature_number, {})
        vendor_status = feature.get("vendors", {})
        row = [f"Feature {feature_number}"]
        for vendor in vendors:
            row.append(vendor_status.get(vendor, ""))
        lines.append("| " + " | ".join(row) + " |")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Generate a simplified feature matrix table from the comparison markdown."
    )
    parser.add_argument(
        "--input",
        default="docs/research/feature_compare_matrix.md",
        help="Path to the source comparison markdown.",
    )
    parser.add_argument(
        "--output",
        default="docs/research/feature_compare_matrix_table.md",
        help="Path for the generated markdown table.",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="Print the table to stdout instead of writing a file.",
    )
    args = parser.parse_args()

    input_path = Path(args.input)
    if not input_path.exists():
        raise SystemExit(f"Input not found: {input_path}")

    features, vendors = parse_features(input_path.read_text(encoding="utf-8").splitlines())
    table = format_table(features, vendors)
    legend = (
        "Legend: "
        f"{STATUS_MAP['Provided']} provided, "
        f"{STATUS_MAP['Partially Provided']} partial, "
        f"{STATUS_MAP['Not Provided']} not provided."
    )
    output = legend + "\n\n" + table + "\n"

    if args.stdout:
        print(output)
        return

    output_path = Path(args.output)
    output_path.write_text(output, encoding="utf-8")


if __name__ == "__main__":
    main()
