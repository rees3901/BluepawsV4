import { strict as assert } from "node:assert";
import { test } from "node:test";
import { gnssUncertainty, uncertaintyPolygon } from "./gnssUncertainty.ts";
const device = { hasGps: true, status: "Out" as const, lat: 51, lon: -2,
  gnss: {accuracyM: 5, satellites: 6, fixAgeS: 0, recordedAt: "2026-10-07T10:00:00Z"} };
test("uncertainty preserves the estimate while capping only the drawn radius", () => {
  assert.equal(gnssUncertainty(device)?.radius, 5);
  const poor = gnssUncertainty({...device, gnss:{...device.gnss,accuracyM:100}})!;
  assert.equal(poor.radius,50); assert.equal(poor.capped,true);
  assert.match(poor.label,/100 m.*capped at 50 m/);
  for (const accuracyM of [0,-1,65535,NaN,Infinity]) assert.equal(gnssUncertainty({...device,gnss:{...device.gnss,accuracyM}}),null);
});
test("Home, hubs and unknown locations do not receive a GPS ring", () => {
  assert.equal(gnssUncertainty({...device,status:"Home"}),null);
  assert.equal(gnssUncertainty({...device,entity:"hub"}),null);
  assert.equal(gnssUncertainty({...device,hasGps:false}),null);
  assert.equal(gnssUncertainty({...device,gnss:null}),null);
});
test("geographic rings remain closed and retain their metre radius", () => {
  for (const lat of [0,51,80]) {
    const ring = uncertaintyPolygon(lat,-2,50).coordinates[0];
    assert.equal(ring.length,65); assert.deepEqual(ring[0],ring[64]);
    for (const [lon,y] of ring) {
      const r = Math.PI/180;
      const a = Math.sin((y-lat)*r/2)**2 + Math.cos(lat*r)*Math.cos(y*r)*Math.sin((lon+2)*r/2)**2;
      assert.ok(Math.abs(2*6371000*Math.asin(Math.sqrt(a))-50)<0.001);
    }
  }
});
