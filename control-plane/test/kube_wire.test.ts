import { test } from "node:test";
import assert from "node:assert/strict";
// Тот же сериализатор, которым KubernetesObjectApi.patch превращает объект в тело запроса.
import { ObjectSerializer } from "@kubernetes/client-node/dist/serializer.js";
import { getSerializationType } from "@kubernetes/client-node/dist/util.js";
import { toWire } from "../src/kube.js";
import { desiredObjects, secretManifest, type KubeObject } from "../src/manifests.js";
import { account, config } from "./helpers/fixtures.js";

const ID = "a1b2c3d4e5f";
const objects: KubeObject[] = [
  ...desiredObjects(account(ID, { status: "active", archiveEnabled: true }), config()),
  secretManifest(ID, { clientToken: `${ID}.C`, adminToken: "A", mcpServiceToken: "M", archiverServiceToken: "R", databaseEncryptionKey: "K" }, 1, config()),
];

const onWire = (o: object & { apiVersion: string; kind: string }) =>
  JSON.parse(JSON.stringify(ObjectSerializer.serialize(o, getSerializationType(o.apiVersion, o.kind))));

test("raw objects lose fields in client-node serialization (NetworkPolicy from)", () => {
  const np = objects.find((o) => o.kind === "NetworkPolicy")!;
  assert.equal(onWire(np).spec.ingress[0].from, undefined);
});

test("toWire: every manifest survives serialization unchanged", () => {
  for (const o of objects) {
    assert.deepEqual(onWire(toWire(o)), JSON.parse(JSON.stringify(o)), o.kind);
  }
});
