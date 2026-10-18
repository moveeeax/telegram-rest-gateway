import { test, describe, before, after } from "node:test";
import assert from "node:assert/strict";
import type { Store } from "../src/store.js";
import { MemStore } from "../src/store_mem.js";
import { PgStore } from "../src/store_pg.js";
import { account } from "./helpers/fixtures.js";

const DAY = 24 * 3600 * 1000;
const MIN15 = 15 * 60 * 1000;

function suite(name: string, make: () => Promise<Store>, cleanup?: () => Promise<void>) {
  describe(name, () => {
    let s: Store;
    const now = new Date("2026-10-18T12:00:00Z");
    before(async () => { s = await make(); });
    after(async () => { await cleanup?.(); });

    test("claim: second session gets in_use, expiry, used", async () => {
      await s.createInvite({ codeHash: "c1", kind: "new", accountId: null, ttlMs: DAY, now });
      const a = await s.claimInvite("c1", "s1", MIN15, 3, now);
      assert.equal(a.ok, true);
      const b = await s.claimInvite("c1", "s2", MIN15, 3, now);
      assert.deepEqual(b, { ok: false, reason: "in_use" });
      await s.markInviteUsed("c1", now);
      assert.deepEqual(await s.claimInvite("c1", "s3", MIN15, 3, now), { ok: false, reason: "used" });
      await s.createInvite({ codeHash: "c2", kind: "new", accountId: null, ttlMs: DAY, now });
      const later = new Date(now.getTime() + DAY + 1);
      assert.deepEqual(await s.claimInvite("c2", "s4", MIN15, 3, later), { ok: false, reason: "expired" });
      assert.deepEqual(await s.claimInvite("nope", "s5", MIN15, 3, now), { ok: false, reason: "not_found" });
    });

    test("claim: same session reclaims its own invite", async () => {
      await s.createInvite({ codeHash: "same", kind: "new", accountId: null, ttlMs: DAY, now });
      assert.equal((await s.claimInvite("same", "sx", MIN15, 3, now)).ok, true);
      assert.equal((await s.claimInvite("same", "sx", MIN15, 3, now)).ok, true);
      await s.releaseInvite("same");
    });

    test("busy: fourth concurrent onboarding is refused and stays unclaimed", async () => {
      for (const c of ["b1", "b2", "b3", "b4"]) {
        await s.createInvite({ codeHash: c, kind: "new", accountId: null, ttlMs: DAY, now });
      }
      for (const c of ["b1", "b2", "b3"]) assert.equal((await s.claimInvite(c, "s-" + c, MIN15, 3, now)).ok, true);
      assert.deepEqual(await s.claimInvite("b4", "s-b4", MIN15, 3, now), { ok: false, reason: "busy" });
      assert.equal((await s.getInvite("b4"))?.claimedAt, null);
      await s.releaseInvite("b1");
      assert.equal((await s.getInvite("b1"))?.claimedAt, null);
      assert.equal((await s.claimInvite("b4", "s-b4", MIN15, 3, now)).ok, true);
      for (const c of ["b2", "b3", "b4"]) await s.releaseInvite(c);
    });

    test("parallel claims of one code: exactly one wins", async () => {
      await s.createInvite({ codeHash: "p1", kind: "new", accountId: null, ttlMs: DAY, now });
      const rs = await Promise.all([1, 2, 3, 4].map((i) => s.claimInvite("p1", "ps" + i, MIN15, 3, now)));
      assert.equal(rs.filter((r) => r.ok).length, 1);
      await s.releaseInvite("p1");
    });

    test("inviteBySession, attachAccount, expiredSessions", async () => {
      await s.createInvite({ codeHash: "e1", kind: "new", accountId: null, ttlMs: DAY, now });
      await s.claimInvite("e1", "se1", MIN15, 3, now);
      await s.attachAccount("e1", "aaaaaaaaaa1");
      assert.equal((await s.inviteBySession("se1", now))?.accountId, "aaaaaaaaaa1");
      assert.equal(await s.inviteBySession("se1", new Date(now.getTime() + MIN15 + 1)), null);
      const exp = await s.expiredSessions(new Date(now.getTime() + MIN15 + 1));
      assert.ok(exp.some((i) => i.codeHash === "e1"));
      await s.releaseInvite("e1");
    });

    test("bumpAttempt counts per kind", async () => {
      await s.createInvite({ codeHash: "t1", kind: "new", accountId: null, ttlMs: DAY, now });
      assert.equal(await s.bumpAttempt("t1", "code"), 1);
      assert.equal(await s.bumpAttempt("t1", "code"), 2);
      assert.equal(await s.bumpAttempt("t1", "code"), 3);
      assert.equal(await s.bumpAttempt("t1", "password"), 1);
    });

    test("outcome and one-time token show", async () => {
      await s.createInvite({ codeHash: "o1", kind: "new", accountId: null, ttlMs: DAY, now });
      await s.setOutcome("o1", "duplicate");
      assert.equal((await s.getInvite("o1"))?.outcome, "duplicate");
      await s.insertAccount(account("aaaaaaaaaa3"));
      assert.equal(await s.claimTokenShow("aaaaaaaaaa3", now), true);
      assert.equal(await s.claimTokenShow("aaaaaaaaaa3", now), false);
      assert.deepEqual((await s.getAccount("aaaaaaaaaa3"))?.tokenShownAt, now);
      await s.deleteAccount("aaaaaaaaaa3");
    });

    test("accounts CRUD and activeByTelegramId ignores needs_relogin", async () => {
      await s.insertAccount(account("aaaaaaaaaa2"));
      await s.updateAccount("aaaaaaaaaa2", { status: "needs_relogin", telegramUserId: "777" });
      assert.equal(await s.activeByTelegramId("777"), null);
      await s.updateAccount("aaaaaaaaaa2", { status: "active", secretRev: 2 });
      const got = await s.activeByTelegramId("777");
      assert.equal(got?.accountId, "aaaaaaaaaa2");
      assert.equal(got?.secretRev, 2);
      assert.ok((await s.listAccounts()).some((a) => a.accountId === "aaaaaaaaaa2"));
      await s.deleteAccount("aaaaaaaaaa2");
      assert.equal(await s.getAccount("aaaaaaaaaa2"), null);
    });
  });
}

suite("MemStore", async () => new MemStore());

const pgUrl = process.env.PG_TEST_URL;
if (pgUrl) {
  let pg: PgStore;
  suite("PgStore", async () => {
    pg = await PgStore.connect(pgUrl);
    await pg.resetForTests();
    return pg;
  }, async () => { await pg.close(); });
} else {
  test("PgStore", { skip: "PG_TEST_URL not set" }, () => {});
}
