// SPDX-License-Identifier: GPL-3.0-or-later
// NX-Nexus -- streaming installer tests against a mock backend.
//
// The point of the abstract backend is right here: these exercise the whole
// install sequence, including rollback on a mid-transfer failure, without a
// console and without ncm. Chunk-boundary handling gets particular attention
// because in production the chunk sizes are whatever USB happens to deliver.

#include <string.h>

#include "nexus_test.h"
#include "fixtures.h"
#include "nexus/installer.h"

#define TITLE_ID 0x0100000000010000ull

// Content ids, matching what fixtureBuildCnmt writes (0xA0 + index) so the
// CNMT and the NSP filenames agree.
#define META_ID_HEX    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PROGRAM_ID_HEX "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1"
#define CONTROL_ID_HEX "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2"

// ---------------------------------------------------------------------------
// Mock backend
// ---------------------------------------------------------------------------

#define MOCK_MAX_CONTENTS 16

typedef struct {
    u8     id[NEXUS_CONTENT_ID_SIZE];
    u64    declared_size;
    size_t written;
    bool   committed;
} MockContent;

typedef struct {
    MockContent contents[MOCK_MAX_CONTENTS];
    u32         content_count;
    bool        content_open;

    bool ticket_imported;
    size_t tik_len, cert_len;

    bool meta_registered;
    bool record_pushed;
    bool rolled_back;

    NexusInstallMeta seen_meta;

    // The CNMT this backend hands back from the registered meta NCA.
    const u8 *cnmt;
    size_t    cnmt_len;

    // Fault injection: fail the Nth call to the named hook (0 == never).
    u32 fail_content_begin_on;
    u32 fail_content_write_on;
    u32 fail_read_cnmt;
    u32 fail_register_meta;
    u32 fail_push_record;

    u32 n_begin, n_write;
} MockBackend;

static int mock_content_begin(void *user, const u8 id[NEXUS_CONTENT_ID_SIZE], u64 size)
{
    MockBackend *m = (MockBackend *)user;
    m->n_begin++;
    if (m->fail_content_begin_on == m->n_begin) return -1;
    if (m->content_count >= MOCK_MAX_CONTENTS)  return -1;

    MockContent *c = &m->contents[m->content_count++];
    memcpy(c->id, id, NEXUS_CONTENT_ID_SIZE);
    c->declared_size = size;
    c->written       = 0;
    c->committed     = false;
    m->content_open  = true;
    return 0;
}

static int mock_content_write(void *user, const void *data, size_t len)
{
    MockBackend *m = (MockBackend *)user;
    m->n_write++;
    if (m->fail_content_write_on == m->n_write) return -1;
    if (!m->content_open) return -1;

    (void)data;
    m->contents[m->content_count - 1].written += len;
    return 0;
}

static int mock_content_commit(void *user)
{
    MockBackend *m = (MockBackend *)user;
    if (!m->content_open) return -1;
    m->contents[m->content_count - 1].committed = true;
    m->content_open = false;
    return 0;
}

static void mock_content_discard(void *user)
{
    MockBackend *m = (MockBackend *)user;
    if (m->content_open && m->content_count > 0) m->content_count--;
    m->content_open = false;
}

static int mock_read_cnmt(void *user, const u8 meta_id[NEXUS_CONTENT_ID_SIZE],
                          void *out, size_t cap, size_t *out_len)
{
    MockBackend *m = (MockBackend *)user;
    (void)meta_id;
    if (m->fail_read_cnmt) return -1;
    if (m->cnmt == NULL || m->cnmt_len > cap) return -1;

    memcpy(out, m->cnmt, m->cnmt_len);
    *out_len = m->cnmt_len;
    return 0;
}

static int mock_import_ticket(void *user, const void *tik, size_t tik_len,
                              const void *cert, size_t cert_len)
{
    MockBackend *m = (MockBackend *)user;
    (void)tik; (void)cert;
    m->ticket_imported = true;
    m->tik_len  = tik_len;
    m->cert_len = cert_len;
    return 0;
}

static int mock_register_meta(void *user, const NexusInstallMeta *meta)
{
    MockBackend *m = (MockBackend *)user;
    if (m->fail_register_meta) return -1;
    m->meta_registered = true;
    m->seen_meta = *meta;
    return 0;
}

static int mock_push_record(void *user, const NexusInstallMeta *meta)
{
    MockBackend *m = (MockBackend *)user;
    (void)meta;
    if (m->fail_push_record) return -1;
    m->record_pushed = true;
    return 0;
}

static void mock_rollback(void *user, const NexusInstallMeta *meta)
{
    MockBackend *m = (MockBackend *)user;
    (void)meta;
    m->rolled_back = true;
}

static const NexusInstallBackendOps kMockOps = {
    .content_begin   = mock_content_begin,
    .content_write   = mock_content_write,
    .content_commit  = mock_content_commit,
    .content_discard = mock_content_discard,
    .read_cnmt       = mock_read_cnmt,
    .import_ticket   = mock_import_ticket,
    .register_meta   = mock_register_meta,
    .push_record     = mock_push_record,
    .rollback        = mock_rollback,
};

// ---------------------------------------------------------------------------
// Shared fixture: a small but structurally complete NSP
// ---------------------------------------------------------------------------

// These are large enough that they must be heap/static rather than stack.
static u8 g_nsp[32768];
static u8 g_cnmt[1024];
static u8 g_tik[1024];
static NexusInstaller g_ins;

static u8 g_meta_nca[512];
static u8 g_prog_nca[4096];
static u8 g_ctrl_nca[1024];
static u8 g_cert[768];

// Builds the NSP and the CNMT the mock will return. include_ticket controls
// whether .tik/.cert entries are present.
static size_t build_nsp(MockBackend *m, bool include_ticket, bool include_control)
{
    memset(g_meta_nca, 0x11, sizeof(g_meta_nca));
    memset(g_prog_nca, 0x22, sizeof(g_prog_nca));
    memset(g_ctrl_nca, 0x33, sizeof(g_ctrl_nca));
    memset(g_cert,     0x44, sizeof(g_cert));

    const size_t tik_len = fixtureBuildTicket(g_tik, sizeof(g_tik),
                                              TicketSigType_Rsa2048Sha256,
                                              TicketKeyType_Common, TITLE_ID, 0);

    // The CNMT lists everything except the meta NCA, which never describes
    // itself. Content ids follow fixtureBuildCnmt's 0xA0+index convention.
    const u8  types[] = { CnmtContentType_Program, CnmtContentType_Control };
    const u64 sizes[] = { sizeof(g_prog_nca), sizeof(g_ctrl_nca) };
    const u16 n = include_control ? 2 : 1;

    // Entry 0 of the CNMT gets id 0xA0.., so shift by one: build with a dummy
    // leading entry would be confusing, so instead the NSP names are chosen to
    // match 0xA1 (program) and 0xA2 (control).
    const u8  types_shifted[] = { CnmtContentType_Meta, CnmtContentType_Program, CnmtContentType_Control };
    const u64 sizes_shifted[] = { sizeof(g_meta_nca), sizeof(g_prog_nca), sizeof(g_ctrl_nca) };
    (void)types; (void)sizes;

    // Build with a leading Meta entry so indices line up with 0xA0/0xA1/0xA2,
    // then drop it: the installer must add the meta entry itself.
    const size_t full = fixtureBuildCnmt(g_cnmt, sizeof(g_cnmt), TITLE_ID, 0x10000,
                                         CnmtMetaType_Application, 0x10,
                                         types_shifted, sizes_shifted,
                                         (u16)(n + 1));
    // Rewrite the content count to skip the leading Meta entry, and shift the
    // array down so the CNMT contains only Program (+ Control).
    const size_t ci_off = CNMT_HEADER_SIZE + 0x10;
    memmove(g_cnmt + ci_off, g_cnmt + ci_off + CNMT_CONTENT_INFO_SIZE,
            (size_t)n * CNMT_CONTENT_INFO_SIZE);
    g_cnmt[0x10] = (u8)n;
    g_cnmt[0x11] = 0;

    m->cnmt     = g_cnmt;
    m->cnmt_len = full - CNMT_CONTENT_INFO_SIZE;

    FixtureNspFile files[5];
    u32 count = 0;

    files[count++] = (FixtureNspFile){ META_ID_HEX ".cnmt.nca", g_meta_nca, sizeof(g_meta_nca) };
    files[count++] = (FixtureNspFile){ PROGRAM_ID_HEX ".nca",   g_prog_nca, sizeof(g_prog_nca) };
    if (include_control) {
        files[count++] = (FixtureNspFile){ CONTROL_ID_HEX ".nca", g_ctrl_nca, sizeof(g_ctrl_nca) };
    }
    if (include_ticket) {
        files[count++] = (FixtureNspFile){ "rights.tik",  g_tik,  tik_len };
        files[count++] = (FixtureNspFile){ "rights.cert", g_cert, sizeof(g_cert) };
    }

    return fixtureBuildNsp(g_nsp, sizeof(g_nsp), files, count);
}

// Feeds the NSP in fixed-size chunks to exercise boundary handling.
static NexusInstallResult feed_chunked(NexusInstaller *ins, const u8 *data,
                                       size_t len, size_t chunk)
{
    NexusInstallResult r = NexusInstall_InProgress;
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        r = nexusInstallFeed(ins, data + off, n);
        if (r != NexusInstall_InProgress && r != NexusInstall_Ok) return r;
    }
    return r;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void t_classify(void)
{
    u8 id[NEXUS_CONTENT_ID_SIZE];

    CHECK_U64(nexusInstallClassify(META_ID_HEX ".cnmt.nca", id), NexusEntryKind_MetaNca);
    CHECK_U64(id[0], 0xAA);

    CHECK_U64(nexusInstallClassify(PROGRAM_ID_HEX ".nca", id), NexusEntryKind_Nca);
    CHECK_U64(id[0], 0xA1);

    CHECK_U64(nexusInstallClassify("anything.tik",  id), NexusEntryKind_Ticket);
    CHECK_U64(nexusInstallClassify("anything.cert", id), NexusEntryKind_Cert);
    CHECK_U64(nexusInstallClassify("junk.xml",      id), NexusEntryKind_Ignore);

    // An NCA whose stem is not 32 hex characters is not usable content.
    CHECK_U64(nexusInstallClassify("short.nca", id), NexusEntryKind_Ignore);
    CHECK_U64(nexusInstallClassify("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz.nca", id),
              NexusEntryKind_Ignore);
}

static void t_parse_content_id(void)
{
    u8 id[NEXUS_CONTENT_ID_SIZE];

    CHECK(nexusInstallParseContentId("0123456789abcdef0123456789ABCDEF", 32, id),
          "valid hex should parse");
    CHECK_U64(id[0], 0x01);
    CHECK_U64(id[15], 0xEF);

    CHECK(!nexusInstallParseContentId("0123", 4, id), "short id must be rejected");
    CHECK(!nexusInstallParseContentId("0123456789abcdef0123456789abcdeg", 32, id),
          "non-hex must be rejected");
}

static void t_install_happy_path(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);
    CHECK(n > 0, "nsp fixture build failed");

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    CHECK_U64(nexusInstallFeed(&g_ins, g_nsp, n), NexusInstall_Ok);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_Ok);

    // Three NCAs opened, written in full and committed.
    CHECK_U64(m.content_count, 3);
    for (u32 i = 0; i < m.content_count; i++) {
        CHECK(m.contents[i].committed, "content %u not committed", i);
        CHECK_U64(m.contents[i].written, m.contents[i].declared_size);
    }

    CHECK(m.ticket_imported, "ticket should have been imported");
    CHECK(m.meta_registered, "meta should have been registered");
    CHECK(m.record_pushed, "application record should have been pushed");
    CHECK(!m.rolled_back, "must not roll back on success");

    // The meta record carries the Meta NCA plus everything the CNMT listed.
    CHECK_U64(m.seen_meta.title_id, TITLE_ID);
    CHECK_U64(m.seen_meta.version, 0x10000);
    CHECK_U64(m.seen_meta.meta_type, CnmtMetaType_Application);
    CHECK_U64(m.seen_meta.storage_id, NexusInstallTarget_SdCard);
    CHECK_U64(m.seen_meta.content_count, 3);
    CHECK_U64(m.seen_meta.contents[0].content_type, CnmtContentType_Meta);
    CHECK_U64(m.seen_meta.contents[0].size, sizeof(g_meta_nca));
    CHECK_U64(m.seen_meta.ext_header_size, 0x10);
    CHECK(m.seen_meta.has_ticket, "ticket details should be recorded");
}

static void t_chunk_boundaries(void)
{
    // The installer must behave identically no matter how the stream is split.
    // 1 byte at a time is the pathological case; 7 and 13 land mid-header and
    // mid-entry; a chunk larger than the whole NSP is the trivial case.
    const size_t chunks[] = { 1, 7, 13, 64, 1000, 4096, 65536 };

    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        MockBackend m; memset(&m, 0, sizeof(m));
        const size_t n = build_nsp(&m, true, true);

        nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
        const NexusInstallResult r = feed_chunked(&g_ins, g_nsp, n, chunks[i]);
        CHECK(r == NexusInstall_Ok, "chunk %zu: feed returned '%s'",
              chunks[i], nexusInstallStr(r));
        CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_Ok);

        CHECK(m.content_count == 3, "chunk %zu: expected 3 contents, got %u",
              chunks[i], m.content_count);
        for (u32 c = 0; c < m.content_count; c++) {
            CHECK(m.contents[c].written == m.contents[c].declared_size,
                  "chunk %zu: content %u wrote %zu of %llu", chunks[i], c,
                  m.contents[c].written,
                  (unsigned long long)m.contents[c].declared_size);
        }
        CHECK(m.record_pushed, "chunk %zu: record not pushed", chunks[i]);
    }
}

static void t_bytes_written_accounting(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    nexusInstallFeed(&g_ins, g_nsp, n);

    // Only NCA bytes count as content; ticket, cert and header do not.
    CHECK_U64(nexusInstallGetBytesWritten(&g_ins),
              sizeof(g_meta_nca) + sizeof(g_prog_nca) + sizeof(g_ctrl_nca));
}

static void t_no_meta_nca(void)
{
    // An NSP with no *.cnmt.nca cannot be installed: there is nothing to
    // read the content list from.
    static u8 nsp[4096];
    static u8 nca[256];
    memset(nca, 0x55, sizeof(nca));

    const FixtureNspFile files[] = {
        { PROGRAM_ID_HEX ".nca", nca, sizeof(nca) },
    };
    const size_t n = fixtureBuildNsp(nsp, sizeof(nsp), files, 1);

    MockBackend m; memset(&m, 0, sizeof(m));
    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    CHECK_U64(nexusInstallFeed(&g_ins, nsp, n), NexusInstall_NoMetaNca);
    CHECK(m.rolled_back, "rollback should run after a fatal header error");
}

static void t_bad_container(void)
{
    static u8 junk[512];
    memset(junk, 0xFF, sizeof(junk));

    MockBackend m; memset(&m, 0, sizeof(m));
    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    CHECK_U64(nexusInstallFeed(&g_ins, junk, sizeof(junk)), NexusInstall_BadContainer);
}

static void t_backend_failure_rolls_back(void)
{
    // A placeholder that cannot be created must abort the install and clean up
    // rather than leaving half a title registered.
    MockBackend m; memset(&m, 0, sizeof(m));
    build_nsp(&m, true, true);
    m.fail_content_begin_on = 2;   // fail on the second NCA

    const size_t n = build_nsp(&m, true, true);
    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);

    const NexusInstallResult r = feed_chunked(&g_ins, g_nsp, n, 512);
    CHECK_U64(r, NexusInstall_BackendError);
    CHECK(m.rolled_back, "rollback must run after a backend failure");
    CHECK(!m.record_pushed, "record must not be pushed after failure");
}

static void t_write_failure_mid_stream(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);
    m.fail_content_write_on = 3;

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    const NexusInstallResult r = feed_chunked(&g_ins, g_nsp, n, 256);
    CHECK_U64(r, NexusInstall_BackendError);
    CHECK(m.rolled_back, "rollback must run");
    CHECK(!m.content_open, "the open placeholder must be discarded");
}

static void t_abort_mid_stream(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    nexusInstallFeed(&g_ins, g_nsp, n / 2);   // cable pulled halfway
    nexusInstallAbort(&g_ins);

    CHECK(m.rolled_back, "abort must roll back");
    CHECK(!m.record_pushed, "no record after an abort");

    // Further feeds after an abort must be refused, not silently resumed.
    CHECK_U64(nexusInstallFeed(&g_ins, g_nsp, 16), NexusInstall_Aborted);
}

static void t_finish_before_stream_ends(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    nexusInstallFeed(&g_ins, g_nsp, n / 3);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_NotFinished);
    CHECK(m.rolled_back, "an early finish must roll back");
}

static void t_cnmt_lists_missing_content(void)
{
    // The CNMT claims a Control NCA, but the NSP only carried the Program.
    // Registering that would produce a title referencing content that is not
    // on the console.
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, false);   // NSP without the control NCA

    // Rebuild the CNMT so it still lists two contents.
    const u8  types[]  = { CnmtContentType_Meta, CnmtContentType_Program, CnmtContentType_Control };
    const u64 sizes[]  = { sizeof(g_meta_nca), sizeof(g_prog_nca), sizeof(g_ctrl_nca) };
    const size_t full  = fixtureBuildCnmt(g_cnmt, sizeof(g_cnmt), TITLE_ID, 0,
                                          CnmtMetaType_Application, 0x10, types, sizes, 3);
    const size_t ci_off = CNMT_HEADER_SIZE + 0x10;
    memmove(g_cnmt + ci_off, g_cnmt + ci_off + CNMT_CONTENT_INFO_SIZE,
            2 * CNMT_CONTENT_INFO_SIZE);
    g_cnmt[0x10] = 2;
    m.cnmt_len = full - CNMT_CONTENT_INFO_SIZE;

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    CHECK_U64(nexusInstallFeed(&g_ins, g_nsp, n), NexusInstall_Ok);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_MissingContent);
    CHECK(m.rolled_back, "must roll back when content is missing");
}

static void t_cnmt_read_failure(void)
{
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);
    m.fail_read_cnmt = 1;

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    nexusInstallFeed(&g_ins, g_nsp, n);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_BackendError);
    CHECK(m.rolled_back, "must roll back when the cnmt cannot be read");
}

static void t_push_record_failure(void)
{
    // The content is all in place but the record push fails. This must still
    // roll back -- otherwise the console keeps orphaned content forever.
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, true, true);
    m.fail_push_record = 1;

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_SdCard);
    nexusInstallFeed(&g_ins, g_nsp, n);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_BackendError);
    CHECK(m.meta_registered, "meta was registered before the failure");
    CHECK(m.rolled_back, "must roll back after a failed record push");
}

static void t_install_without_ticket(void)
{
    // Titles that need no title key ship no ticket; that is normal, not an error.
    MockBackend m; memset(&m, 0, sizeof(m));
    const size_t n = build_nsp(&m, false, true);

    nexusInstallBegin(&g_ins, &kMockOps, &m, NexusInstallTarget_BuiltInUser);
    CHECK_U64(nexusInstallFeed(&g_ins, g_nsp, n), NexusInstall_Ok);
    CHECK_U64(nexusInstallFinish(&g_ins), NexusInstall_Ok);

    CHECK(!m.ticket_imported, "no ticket should have been imported");
    CHECK(m.record_pushed, "record should still be pushed");
    CHECK_U64(m.seen_meta.storage_id, NexusInstallTarget_BuiltInUser);
    CHECK(!m.seen_meta.has_ticket, "meta should record that there is no ticket");
}

void test_installer(void)
{
    nexusTestRun("entry classification",       t_classify);
    nexusTestRun("content id parsing",         t_parse_content_id);
    nexusTestRun("install happy path",         t_install_happy_path);
    nexusTestRun("arbitrary chunk boundaries", t_chunk_boundaries);
    nexusTestRun("bytes written accounting",   t_bytes_written_accounting);
    nexusTestRun("no meta nca",                t_no_meta_nca);
    nexusTestRun("bad container",              t_bad_container);
    nexusTestRun("backend failure rolls back", t_backend_failure_rolls_back);
    nexusTestRun("write failure mid stream",   t_write_failure_mid_stream);
    nexusTestRun("abort mid stream",           t_abort_mid_stream);
    nexusTestRun("finish before stream ends",  t_finish_before_stream_ends);
    nexusTestRun("cnmt lists missing content", t_cnmt_lists_missing_content);
    nexusTestRun("cnmt read failure",          t_cnmt_read_failure);
    nexusTestRun("push record failure",        t_push_record_failure);
    nexusTestRun("install without ticket",     t_install_without_ticket);
}
