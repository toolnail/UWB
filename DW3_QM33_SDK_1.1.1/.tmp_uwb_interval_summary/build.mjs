import fs from "node:fs/promises";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const outputDir = "outputs/uwb_interval_summary_20260930";
await fs.mkdir(outputDir, { recursive: true });

const workbook = Workbook.create();
const sheet = workbook.worksheets.add("测试结果");
sheet.showGridLines = false;

sheet.getRange("A1:G7").values = [
  ["发送间隔", "发送数", "有效接收", "丢包+错误数", "丢包率+错误率", "成功率", "实测约耗时"],
  ["10 ms", 300000, 299822, 489, null, null, "50分03.870秒"],
  ["5 ms", 300000, 299905, 191, null, null, "26分33.919秒"],
  ["4 ms", 300000, 299888, 225, null, null, "21分39.105秒"],
  ["3 ms", 300000, 299914, 175, null, null, "16分43.817秒"],
  ["2 ms", 300000, 299866, 269, null, null, "11分46.987秒"],
  ["1 ms", 300000, 299896, 210, null, null, "6分52.440秒"],
];
sheet.getRange("E2").formulas = [["=D2/B2"]];
sheet.getRange("E2:E7").fillDown();
sheet.getRange("F2").formulas = [["=1-E2"]];
sheet.getRange("F2:F7").fillDown();

sheet.getRange("A1:G7").format = {
  fill: "#E8EAF4",
  font: { name: "Arial", size: 13, color: "#111827" },
  verticalAlignment: "center",
  borders: { preset: "all", style: "thin", color: "#FFFFFF" },
};
sheet.getRange("A1:G1").format = {
  fill: "#E8EAF4",
  font: { name: "Arial", size: 13, bold: true, color: "#111827" },
  horizontalAlignment: "center",
  verticalAlignment: "center",
  wrapText: true,
  borders: { preset: "all", style: "thin", color: "#FFFFFF" },
};
sheet.getRange("F1:F7").format.fill = "#FFF200";
sheet.getRange("B2:F7").format.horizontalAlignment = "center";
sheet.getRange("A2:A7").format.horizontalAlignment = "left";
sheet.getRange("G2:G7").format.horizontalAlignment = "center";
sheet.getRange("B2:D7").format.numberFormat = "#,##0";
sheet.getRange("E2:F7").format.numberFormat = "0.000000%";
sheet.getRange("A1:G7").format.rowHeight = 30;
sheet.getRange("A1:G1").format.rowHeight = 38;
sheet.getRange("A:A").format.columnWidth = 15;
sheet.getRange("B:B").format.columnWidth = 15;
sheet.getRange("C:C").format.columnWidth = 15;
sheet.getRange("D:D").format.columnWidth = 19;
sheet.getRange("E:E").format.columnWidth = 20;
sheet.getRange("F:F").format.columnWidth = 16;
sheet.getRange("G:G").format.columnWidth = 21;

sheet.getRange("A9:A11").values = [
  ["口径：丢包+错误数 = lost + phy_err + invalid；成功率 = (发送数 − 失败数) / 发送数。"],
  ["注意：phy_err 可能与 lost 对应同一丢失数据包；按要求直接相加可能重复计数。"],
  ["来源：interval_packet_loss_summary.csv；耗时沿用用户提供表格中的实测值。"],
];
sheet.getRange("A9:G11").format = {
  font: { name: "Arial", size: 10, color: "#374151" },
  verticalAlignment: "center",
  wrapText: false,
};
sheet.getRange("A9:G11").format.rowHeight = 24;

workbook.recalculate();
const inspected = await workbook.inspect({
  kind: "table",
  range: "测试结果!A1:G7",
  include: "values,formulas",
  tableMaxRows: 10,
  tableMaxCols: 10,
  maxChars: 4000,
});
console.log(inspected.ndjson);
const preview = await workbook.render({ sheetName: "测试结果", range: "A1:G11", scale: 1.5, format: "png" });
await fs.writeFile(".tmp_uwb_interval_summary/preview.png", new Uint8Array(await preview.arrayBuffer()));
const xlsx = await SpreadsheetFile.exportXlsx(workbook);
await xlsx.save(`${outputDir}/uwb_点对点通信统计.xlsx`);
console.log(`Saved ${outputDir}/uwb_点对点通信统计.xlsx`);

