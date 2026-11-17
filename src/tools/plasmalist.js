var out = [];
for (var i = 0; i < screenCount; i++) { var g = screenGeometry(i); out.push("screen " + i + ": " + g.x + "," + g.y + " " + g.width + "x" + g.height); }
var ds = desktops();
for (var j = 0; j < ds.length; j++) out.push("desktop id " + ds[j].id + " screen " + ds[j].screen + " " + ds[j].type);
var ps = panels();
for (var k = 0; k < ps.length; k++) out.push("panel id " + ps[k].id + " screen " + ps[k].screen + " " + ps[k].type + " loc " + ps[k].location + " widgets " + ps[k].widgets().map(function (w) { return w.type; }).join(","));
print(out.join("\n"));
