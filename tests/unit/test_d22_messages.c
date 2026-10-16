#include "../../core/src/wire/messages_d22_internal.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void same_bytes(moq_bytes_t actual, moq_bytes_t expected)
{
    CHECK(actual.len==expected.len);
    if (actual.len==expected.len && expected.len)
        CHECK(actual.data && memcmp(actual.data,expected.data,expected.len)==0);
}

static void semantic_fields(const moq_d22_message_t *out,
                            const moq_d22_message_t *expected)
{
    CHECK(out->type==expected->type);
    CHECK(out->request_id==expected->request_id);
    CHECK(out->track_alias==expected->track_alias);
    CHECK(out->ns.count==expected->ns.count);
    CHECK(out->ns.value_bytes==expected->ns.value_bytes);
    same_bytes(out->ns.encoded,expected->ns.encoded);
    same_bytes(out->ns.fields,expected->ns.fields);
    same_bytes(out->track_name,expected->track_name);
    CHECK(out->params.count==0 && out->params.present==0 && out->params.range_count==0);
    CHECK(out->params.encoded.len==0);
    same_bytes(out->properties,expected->properties);
    CHECK(out->code==expected->code);
    CHECK(out->retry_interval==expected->retry_interval);
    CHECK(out->stream_count==expected->stream_count);
    same_bytes(out->reason,expected->reason);
    same_bytes(out->redirect_uri,expected->redirect_uri);
    CHECK(out->end_of_track==expected->end_of_track);
    CHECK(out->end_location.group==expected->end_location.group);
    CHECK(out->end_location.object==expected->end_location.object);
}

static void literals(void)
{
    static const uint8_t ns_n[]={1,1,'n'}, ns_empty[]={0};
    const moq_d22_namespace_t named={.encoded={ns_n,sizeof(ns_n)},
        .fields={ns_n+1,2},.count=1,.value_bytes=1};
    const moq_d22_namespace_t empty={.encoded={ns_empty,sizeof(ns_empty)}};
    const moq_bytes_t track={(const uint8_t *)"t",1};
    static const struct {
        uint64_t type;
        uint64_t request;
        uint8_t body[32];
        size_t len;
    } cases[]={
        {MOQ_D22_SUBSCRIBE,0,{2,1,1,'n',1,'t',0},7},
        {MOQ_D22_FETCH,0,{2,1,1,'n',1,'t',0},7},
        {MOQ_D22_TRACK_STATUS,0,{2,1,1,'n',1,'t',0},7},
        {MOQ_D22_PUBLISH,0,{3,1,1,'n',1,'t',5,0},8},
        {MOQ_D22_SUBSCRIBE_OK,0,{5,0},2},
        {MOQ_D22_FETCH_OK,0,{1,7,9,0},4},
        {MOQ_D22_REQUEST_OK,MOQ_D22_PUBLISH,{0},1},
        {MOQ_D22_REQUEST_ERROR,MOQ_D22_SUBSCRIBE,{1,0,1,'x'},4},
        {MOQ_D22_REQUEST_UPDATE,MOQ_D22_SUBSCRIBE,{4,0},2},
        {MOQ_D22_PUBLISH_STATE_NOTIFY,0,{0},1},
        {MOQ_D22_PUBLISH_NAMESPACE,0,{3,1,1,'n',0},5},
        {MOQ_D22_SUBSCRIBE_NAMESPACE,0,{2,0,0},3},
        {MOQ_D22_SUBSCRIBE_TRACKS,0,{2,0,0},3},
        {MOQ_D22_NAMESPACE,0,{1,1,'n'},3},
        {MOQ_D22_NAMESPACE_DONE,0,{0},1},
        {MOQ_D22_PUBLISH_SKIPPED,0,{0,1,'t'},3},
        {MOQ_D22_PUBLISH_DONE,0,{0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0},11}
    };
    /* Authored independently from the wire and never populated by decode. */
    const moq_d22_message_t expected[]={
        {.type=MOQ_D22_SUBSCRIBE,.request_id=2,.ns=named,.track_name=track},
        {.type=MOQ_D22_FETCH,.request_id=2,.ns=named,.track_name=track},
        {.type=MOQ_D22_TRACK_STATUS,.request_id=2,.ns=named,.track_name=track},
        {.type=MOQ_D22_PUBLISH,.request_id=3,.ns=named,.track_name=track,.track_alias=5},
        {.type=MOQ_D22_SUBSCRIBE_OK,.track_alias=5},
        {.type=MOQ_D22_FETCH_OK,.end_of_track=1,.end_location={7,9}},
        {.type=MOQ_D22_REQUEST_OK},
        {.type=MOQ_D22_REQUEST_ERROR,.code=1,.reason={(const uint8_t *)"x",1}},
        {.type=MOQ_D22_REQUEST_UPDATE,.request_id=4},
        {.type=MOQ_D22_PUBLISH_STATE_NOTIFY},
        {.type=MOQ_D22_PUBLISH_NAMESPACE,.request_id=3,.ns=named},
        {.type=MOQ_D22_SUBSCRIBE_NAMESPACE,.request_id=2,.ns=empty},
        {.type=MOQ_D22_SUBSCRIBE_TRACKS,.request_id=2,.ns=empty},
        {.type=MOQ_D22_NAMESPACE,.ns=named},
        {.type=MOQ_D22_NAMESPACE_DONE,.ns=empty},
        {.type=MOQ_D22_PUBLISH_SKIPPED,.ns=empty,.track_name=track},
        {.type=MOQ_D22_PUBLISH_DONE,.stream_count=UINT64_MAX}
    };
    CHECK(sizeof(expected)/sizeof(expected[0])==sizeof(cases)/sizeof(cases[0]));
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        moq_d22_message_context_t ctx={.request_type=cases[i].request,
            .update_from_subscriber=true,.max_filter_ranges=8};
        moq_d22_message_t out={0};
        moq_result_t rc=moq_d22_decode_message(cases[i].type,cases[i].body,cases[i].len,&ctx,&out);
        CHECK(rc==MOQ_OK);
        if (rc!=MOQ_OK) continue;
        semantic_fields(&out,&expected[i]);
        uint8_t encoded[64]; memset(encoded,0xa5,sizeof(encoded));
        moq_buf_writer_t w={encoded,sizeof(encoded),1};
        CHECK(moq_d22_encode_message(&w,&expected[i],NULL,0,&ctx)==MOQ_OK);
        CHECK(w.pos==cases[i].len+4);
        CHECK(encoded[0]==0xa5 && encoded[1]==cases[i].type &&
            encoded[2]==0 && encoded[3]==cases[i].len);
        CHECK(memcmp(encoded+4,cases[i].body,cases[i].len)==0);
        memset(encoded,0xa5,sizeof(encoded)); w.pos=1;
        CHECK(moq_d22_encode_message(&w,&out,NULL,0,&ctx)==MOQ_OK);
        CHECK(w.pos==cases[i].len+4);
        CHECK(encoded[0]==0xa5 && encoded[1]==cases[i].type &&
            encoded[2]==0 && encoded[3]==cases[i].len);
        CHECK(memcmp(encoded+4,cases[i].body,cases[i].len)==0);
        for (size_t cap=0;cap<cases[i].len+3;++cap) {
            w=(moq_buf_writer_t){encoded,cap+1,1};
            CHECK(moq_d22_encode_message(&w,&out,NULL,0,&ctx)==MOQ_ERR_BUFFER);
            CHECK(w.pos==1 && encoded[0]==0xa5);
        }
        for (size_t n=0;n<cases[i].len;++n) {
            moq_d22_message_t before;
            memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
            CHECK(moq_d22_decode_message(cases[i].type,cases[i].body,n,&ctx,&out)==MOQ_ERR_PROTO);
            CHECK(memcmp(&out,&before,sizeof(out))==0);
        }
    }
}

static void full_width_publish_identity(void)
{
    static const uint8_t empty[]={0};
    static const struct { uint64_t request_id,alias; uint8_t wire[16]; } cases[]={
        {UINT64_MAX,5,{0x1d,0,13,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,0,5,0}},
        {3,UINT64_MAX,{0x1d,0,13,3,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0}}
    };
    moq_d22_message_context_t ctx={0};
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        moq_d22_message_t expected={.type=MOQ_D22_PUBLISH,
            .request_id=cases[i].request_id,.track_alias=cases[i].alias,
            .ns={.encoded={empty,1}}};
        moq_d22_message_t out={0};
        CHECK(moq_d22_decode_message(MOQ_D22_PUBLISH,cases[i].wire+3,13,&ctx,&out)==MOQ_OK);
        semantic_fields(&out,&expected);
        uint8_t encoded[16]={0}; moq_buf_writer_t w={encoded,sizeof(encoded),0};
        CHECK(moq_d22_encode_message(&w,&expected,NULL,0,&ctx)==MOQ_OK);
        CHECK(w.pos==sizeof(encoded) && memcmp(encoded,cases[i].wire,sizeof(encoded))==0);
    }
}

static void refusal_precedence(void)
{
    static const struct {
        uint64_t type,max_ranges;
        uint8_t body[32]; size_t len;
        moq_result_t result;
    } cases[]={
        {MOQ_D22_FETCH_OK,8,{0,0,0,1,39,2,0,0},8,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH_OK,8,{0,0,0,1,39,3,0,0x81,0},9,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH_OK,8,{0,0,0,1,40,3,0,3,0},9,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE,8,{2,0,0,1,35,6,1,41,3,0,3,0},12,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,2,39,3,0,0x81,0,5,0},11,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,1,39,3,0,0x81,0,0xee},10,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,2,39,3,0,0x81,0,14,2},11,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,3,39,3,0,0x81,0,14,0,0,1},13,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,0,{2,0,0,2,37,0,7,0},8,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,1,{2,0,0,2,37,4,0,0,0,1,7,0},12,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,3,37,2,0,0,0,2,0,1,7,0},14,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,1,40,3,0,3,0x80},9,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,1,39,5,0,0x81,0,0,0x80},11,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,1,37,12,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,0x80},18,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE,8,{2,0,0,1,35,8,2,39,3,0,0x81,0,5,0},14,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE,8,{2,0,0,2,35,6,1,39,3,0,0x81,0,9,0},14,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,8,{2,0,0,2,39,3,0,0x81,0,14},10,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH_OK,8,{0,0,0,0},4,MOQ_OK},
        {MOQ_D22_FETCH,8,{2,0,0,1,39,3,0,0x81,0},9,MOQ_D22_ERR_INVALID_FILTER},
        {MOQ_D22_FETCH,8,{2,0,0,1,40,3,0,3,0},9,MOQ_D22_ERR_INVALID_FILTER},
        {MOQ_D22_FETCH,8,{2,0,0,2,39,3,0,0x81,0,14,1},11,MOQ_D22_ERR_INVALID_FILTER},
        {MOQ_D22_SUBSCRIBE,8,{2,0,0,1,35,6,1,39,3,0,0x81,0},12,MOQ_D22_ERR_INVALID_FILTER},
        /* REGISTER before request-invalid filter: no cache effects in codec. */
        {MOQ_D22_FETCH,8,{2,0,0,2,3,3,1,7,0,36,3,0,0x81,0},14,MOQ_D22_ERR_INVALID_FILTER},
        {MOQ_D22_FETCH,8,{2,0,0,2,3,3,1,7,0,36,3,0,0x81,0,0xee},15,MOQ_ERR_PROTO}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        moq_d22_message_context_t ctx={.max_filter_ranges=cases[i].max_ranges};
        moq_d22_message_t out,before;
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        moq_result_t rc=moq_d22_decode_message(cases[i].type,cases[i].body,cases[i].len,&ctx,&out);
        if (rc!=cases[i].result)
            fprintf(stderr,"refusal row=%zu actual=%d expected=%d\n",i,rc,cases[i].result);
        CHECK(rc==cases[i].result);
        if (rc!=MOQ_OK) CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
}

static void refusals(void)
{
    static const struct { uint64_t type; uint8_t body[16]; size_t len; moq_result_t rc; } cases[]={
        {MOQ_D22_FETCH_OK,{2,0,0,0},4,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE,{2,0,0,1,33,6},6,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE,{2,0,0,1,33,0,0},7,MOQ_ERR_PROTO},
        {MOQ_D22_REQUEST_OK,{0,2,0},3,MOQ_ERR_PROTO},
        {MOQ_D22_SUBSCRIBE_OK,{1,1,16,1},4,MOQ_ERR_PROTO},
        {MOQ_D22_FETCH,{2,0,0,1,39,3,0,0x81,0},9,MOQ_D22_ERR_INVALID_FILTER},
        {MOQ_D22_SUBSCRIBE,{2,0,0,1,3,1,1},7,MOQ_D22_ERR_KVP_FORMAT},
        {MOQ_D22_PUBLISH_DONE,{0,0,0x84,1},4,MOQ_ERR_PROTO},
        {MOQ_D22_REQUEST_ERROR,{0x34,0,0,0,0,1,'x'},7,MOQ_ERR_PROTO},
        {MOQ_D22_NAMESPACE,{0,0},2,MOQ_ERR_PROTO},
        /* Old standalone FETCH layout: priority/order/type/start/end fields. */
        {MOQ_D22_FETCH,{2,0,0,0x80,0,1,1,0,0,1,0,0},12,MOQ_ERR_PROTO}
    };
    moq_d22_message_context_t ctx={.request_type=MOQ_D22_PUBLISH_NAMESPACE,.max_filter_ranges=8};
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        moq_d22_message_t out,before;
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        CHECK(moq_d22_decode_message(cases[i].type,cases[i].body,cases[i].len,&ctx,&out)==cases[i].rc);
        CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
}

static void parameter_messages(void)
{
    const uint8_t body[]={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                          0,0,2,33,5,2,1,0};
    moq_d22_message_context_t ctx={.max_filter_ranges=1};
    moq_d22_message_t message={0};
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,body,sizeof(body),&ctx,&message)==MOQ_OK);
    CHECK(message.request_id==UINT64_MAX && message.ns.count==0 &&
        message.params.count==2 && message.params.present==
        (MOQ_D22_PARAM_PRESENT(33)|MOQ_D22_PARAM_PRESENT(35)));
    const moq_d22_param_t params[]={
        {.type=33,.value.filter={.type=MOQ_D22_FILTER_NEXT_OBJECT}},
        {.type=35,.value.fill={(const uint8_t *)"",1}}
    };
    uint8_t encoded[64]; moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_message(&w,&message,params,2,&ctx)==MOQ_OK);
    CHECK(w.pos==sizeof(body)+3 && memcmp(encoded+3,body,sizeof(body))==0);
    for (size_t n=0;n<sizeof(body);++n) {
        moq_d22_message_t out,before;
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,body,n,&ctx,&out)==MOQ_ERR_PROTO);
        CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
    const uint8_t max_count[]={2,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,max_count,sizeof(max_count),&ctx,&message)==MOQ_ERR_PROTO);
    const uint8_t max_track[]={2,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,max_track,sizeof(max_track),&ctx,&message)==MOQ_ERR_PROTO);
    const uint8_t ok[]={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,2,8,0,1,
                        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0};
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE_OK,ok,sizeof(ok),&ctx,&message)==MOQ_OK);
    CHECK(message.track_alias==UINT64_MAX && message.params.count==2);
    moq_buf_reader_t r={message.params.encoded.data,message.params.encoded.len,0};
    uint64_t previous=0; moq_d22_param_t p={0};
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_OK && p.type==8 && p.value.integer==0);
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_OK && p.type==9 &&
        p.value.location.group==UINT64_MAX && p.value.location.object==0);
}

static void response_and_update_contexts(void)
{
    const uint64_t requests[]={MOQ_D22_PUBLISH,MOQ_D22_REQUEST_UPDATE,MOQ_D22_TRACK_STATUS,
        MOQ_D22_PUBLISH_NAMESPACE,MOQ_D22_SUBSCRIBE_NAMESPACE,MOQ_D22_SUBSCRIBE_TRACKS};
    const uint8_t expires[]={1,8,0};
    const uint8_t largest[]={1,9,0,0};
    const uint8_t properties[]={0,0x79,1,'x'};
    moq_d22_message_t out={0};
    for (size_t i=0;i<sizeof(requests)/sizeof(requests[0]);++i) {
        moq_d22_message_context_t ctx={.request_type=requests[i]};
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_OK,expires,sizeof(expires),&ctx,&out)==
            (requests[i]==MOQ_D22_TRACK_STATUS ? MOQ_ERR_PROTO : MOQ_OK));
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_OK,largest,sizeof(largest),&ctx,&out)==
            (requests[i]==MOQ_D22_TRACK_STATUS || requests[i]==MOQ_D22_REQUEST_UPDATE ? MOQ_OK : MOQ_ERR_PROTO));
        /* §9.3: populated in TRACK_STATUS_OK; the MUST close names PUBLISH_OK,
         * REQUEST_UPDATE_OK, SUBSCRIBE_NAMESPACE_OK and PUBLISH_NAMESPACE_OK,
         * not SUBSCRIBE_TRACKS_OK, whose properties are read and ignored. */
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_OK,properties,sizeof(properties),&ctx,&out)==
            (requests[i]==MOQ_D22_TRACK_STATUS || requests[i]==MOQ_D22_SUBSCRIBE_TRACKS
                ? MOQ_OK : MOQ_ERR_PROTO));
    }
    const uint64_t updated[]={MOQ_D22_SUBSCRIBE,MOQ_D22_PUBLISH,MOQ_D22_FETCH,
        MOQ_D22_PUBLISH_NAMESPACE,MOQ_D22_SUBSCRIBE_NAMESPACE,MOQ_D22_SUBSCRIBE_TRACKS};
    const uint8_t priority[]={4,1,32,255};
    const uint8_t prefix[]={4,1,52,1,1,'n'};
    const uint8_t ranges[]={4,1,37,2,0,0};
    for (size_t i=0;i<sizeof(updated)/sizeof(updated[0]);++i) {
        moq_d22_message_context_t ctx={.request_type=updated[i],.update_from_subscriber=true,
            .max_filter_ranges=1};
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_UPDATE,priority,sizeof(priority),&ctx,&out)==
            (i<3 ? MOQ_OK : MOQ_ERR_PROTO));
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_UPDATE,prefix,sizeof(prefix),&ctx,&out)==
            (i>3 ? MOQ_OK : MOQ_ERR_PROTO));
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_UPDATE,ranges,sizeof(ranges),&ctx,&out)==
            (i<2 ? MOQ_OK : MOQ_ERR_PROTO));
        ctx.update_from_subscriber=false;
        CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_UPDATE,ranges,sizeof(ranges),&ctx,&out)==MOQ_ERR_PROTO);
    }
    const uint8_t redirect[]={0x34,0,0,1,'u',1,1,'n',1,'t'};
    moq_d22_message_context_t ctx={.request_type=MOQ_D22_SUBSCRIBE};
    CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_ERROR,redirect,sizeof(redirect),&ctx,&out)==MOQ_OK);
    CHECK(out.redirect_uri.len==1 && out.redirect_uri.data[0]=='u' && out.track_name.data[0]=='t');
    ctx.request_type=MOQ_D22_REQUEST_UPDATE; ctx.updated_request_type=MOQ_D22_SUBSCRIBE_NAMESPACE;
    CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_ERROR,redirect,sizeof(redirect),&ctx,&out)==MOQ_ERR_PROTO);
    ctx.request_type=MOQ_D22_TRACK_STATUS;
    CHECK(moq_d22_decode_message(MOQ_D22_REQUEST_UPDATE,priority,sizeof(priority),&ctx,&out)==MOQ_ERR_INVAL);
}

static void limits_and_properties(void)
{
    static uint8_t encoded[65548], storage[65540], ns_bytes[4104];
    const uint8_t zero_ns[]={0};
    moq_d22_message_context_t ctx={.request_type=MOQ_D22_SUBSCRIBE};
    moq_d22_message_t m={.type=MOQ_D22_REQUEST_ERROR,.reason={storage,1024}};
    moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_OK);
    moq_d22_message_t out={0};
    CHECK(moq_d22_decode_message(m.type,encoded+3,w.pos-3,&ctx,&out)==MOQ_OK);
    CHECK(out.reason.len==1024);
    m.reason.len=1025; w.pos=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_ERR_INVAL && w.pos==0);
    m=(moq_d22_message_t){.type=MOQ_D22_REQUEST_ERROR,.code=MOQ_D22_ERROR_REDIRECT,
        .redirect_uri={storage,8193},.ns={.encoded={zero_ns,1}}};
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_OK);
    CHECK(moq_d22_decode_message(m.type,encoded+3,w.pos-3,&ctx,&out)==MOQ_OK);
    CHECK(out.redirect_uri.len==8193);
    m.redirect_uri.len=SIZE_MAX; w.pos=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_ERR_INVAL && w.pos==0);
    moq_bytes_t part={storage,4096}; moq_namespace_t ns={&part,1};
    moq_buf_writer_t nw={ns_bytes,sizeof(ns_bytes),0};
    CHECK(moq_d22_encode_namespace(&nw,&ns)==MOQ_OK);
    m=(moq_d22_message_t){.type=MOQ_D22_SUBSCRIBE,
        .ns={.encoded={ns_bytes,nw.pos}},.track_name={(const uint8_t *)"x",1}};
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_ERR_INVAL && w.pos==0);
    m.track_name.len=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_OK);
    const uint8_t props[]={2,0,0x77,1,'x'};
    m=(moq_d22_message_t){.type=MOQ_D22_FETCH_OK,.properties={props,sizeof(props)}};
    w.pos=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_OK);
    CHECK(moq_d22_decode_message(m.type,encoded+3,w.pos-3,&ctx,&out)==MOQ_OK);
    CHECK(out.properties.len==sizeof(props) && memcmp(out.properties.data,props,sizeof(props))==0);
    const uint8_t bad_props[]={0,0,0,0,1,2,'x'};
    CHECK(moq_d22_decode_message(MOQ_D22_FETCH_OK,bad_props,sizeof(bad_props),&ctx,&out)==MOQ_ERR_PROTO);
    /* Exact maximum control body, with one opaque odd property. */
    storage[0]=0x79; storage[1]=0xc0; storage[2]=0xff; storage[3]=0xf7;
    m.properties=(moq_bytes_t){storage,65531}; w.pos=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_OK);
    CHECK(w.pos==65538 && encoded[1]==255 && encoded[2]==255);
    CHECK(moq_d22_decode_message(m.type,encoded+3,65535,&ctx,&out)==MOQ_OK);
    CHECK(moq_d22_decode_message(m.type,encoded+3,65536,&ctx,&out)==MOQ_ERR_PROTO);
    storage[65531]=1; storage[65532]=0; m.properties.len=65533; w.pos=0;
    CHECK(moq_d22_encode_message(&w,&m,NULL,0,&ctx)==MOQ_ERR_INVAL && w.pos==0);
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,NULL,1,&ctx,&out)==MOQ_ERR_INVAL);
    CHECK(moq_d22_decode_message(MOQ_D22_SUBSCRIBE,NULL,0,&ctx,&out)==MOQ_ERR_PROTO);
    CHECK(moq_d22_decode_message(UINT64_MAX,NULL,0,&ctx,&out)==MOQ_ERR_PROTO);
    CHECK(moq_d22_encode_message(&w,&m,NULL,SIZE_MAX,&ctx)==MOQ_ERR_INVAL);
    CHECK(moq_d22_encode_message(NULL,&m,NULL,0,&ctx)==MOQ_ERR_INVAL);
}

int main(void)
{
    literals(); refusals(); parameter_messages(); response_and_update_contexts();
    limits_and_properties();
    full_width_publish_identity(); refusal_precedence();
    printf("draft-22 messages: %d failures\n",failures);
    return failures ? 1 : 0;
}
