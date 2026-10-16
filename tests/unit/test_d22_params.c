#include "../../core/src/wire/params_d22_internal.h"
#include <stdio.h>
#include <string.h>

#ifdef MOQ_D22_COUNT_PARAM_READS
/* CTest d22_params_work: compile this TU without a separate params_d22.c.
 * Counting the primitive reads in the actual source proves work bounds without
 * elapsed-time assertions or any production instrumentation/exported seam. */
static size_t param_vi64_reads;
static moq_result_t counted_read_vi64(moq_buf_reader_t *r, uint64_t *out)
{
    ++param_vi64_reads;
    return moq_buf_read_vi64(r,out);
}
#define moq_buf_read_vi64 counted_read_vi64
#include "../../core/src/wire/params_d22.c"
#undef moq_buf_read_vi64
#endif

static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void literal_params(void)
{
    /* No KVP parity or length prefix around the explicit location filter. */
    const uint8_t wire[] = {2,0, 1,3,3,0,'t', 13,0, 16,255,
                            1,5, 1,2, 1,5,2,10,0,23,0, 18,1};
    const uint8_t fill[] = {2,10,0,23,0};
    moq_d22_param_t in[] = {
        {.type=2, .value.integer=0},
        {.type=3, .value.token={.alias_type=3,.type=0,
            .value={(const uint8_t *)"t",1}}},
        {.type=16, .value.integer=0},
        {.type=32, .value.integer=255},
        {.type=33, .value.filter={.type=MOQ_D22_FILTER_NEXT_OBJECT}},
        {.type=34, .value.integer=2},
        {.type=35, .value.fill={fill,sizeof(fill)}},
        {.type=53, .value.integer=1}
    };
    moq_buf_reader_t r = {wire,sizeof(wire),0};
    moq_d22_params_t out = {0};
    CHECK(moq_d22_decode_params(&r,8,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_OK);
    CHECK(r.pos==sizeof(wire));
    CHECK(out.count==8 && out.encoded.data==wire);
    CHECK((out.present & MOQ_D22_PARAM_PRESENT(2)) != 0);
    moq_buf_reader_t it = {out.encoded.data,out.encoded.len,0};
    uint64_t prev=0;
    moq_d22_param_t p = {0};
    for (size_t i=0;i<8 && it.pos<it.len;++i) {
        CHECK(moq_d22_param_next(&it,&prev,&p)==MOQ_OK);
        CHECK(p.type==in[i].type);
        if (p.type==3) CHECK(p.value.token.value.len==1 &&
            p.value.token.value.data[0]=='t');
        if (p.type==35) {
            moq_d22_params_t inner={0};
            CHECK(moq_d22_decode_fill(p.value.fill,8,&inner)==MOQ_OK);
            CHECK(inner.count==2 && inner.present==
                (MOQ_D22_PARAM_PRESENT(10)|MOQ_D22_PARAM_PRESENT(33)));
        }
    }
    uint8_t encoded[64];
    memset(encoded,0xa5,sizeof(encoded));
    moq_buf_writer_t w={encoded,sizeof(encoded),1};
    CHECK(moq_d22_encode_params(&w,in,8,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_OK);
    CHECK(w.pos==sizeof(wire)+1);
    CHECK(memcmp(encoded+1,wire,sizeof(wire))==0);
    for (size_t n=0;n<sizeof(wire);++n) {
        r=(moq_buf_reader_t){wire,n,0};
        moq_d22_params_t before;
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        CHECK(moq_d22_decode_params(&r,8,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_ERR_BUFFER);
        CHECK(r.pos==0 && memcmp(&before,&out,sizeof(out))==0);
        w=(moq_buf_writer_t){encoded,n+1,1};
        CHECK(moq_d22_encode_params(&w,in,8,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_ERR_BUFFER);
        CHECK(w.pos==1 && encoded[0]==0xa5);
    }
    r=(moq_buf_reader_t){NULL,0,0};
    CHECK(moq_d22_decode_params(&r,0,MOQ_D22_SCOPE_SUBSCRIBE,0,&out)==MOQ_OK);
    CHECK(out.present==0 && out.count==0);
}

static void refusal(const uint8_t *wire, size_t len, uint64_t count,
                    moq_d22_param_scope_t scope, uint64_t max, moq_result_t rc)
{
    moq_buf_reader_t r={wire,len,0};
    moq_d22_params_t out,before;
    memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
    CHECK(moq_d22_decode_params(&r,count,scope,max,&out)==rc);
    CHECK(r.pos==0 && memcmp(&out,&before,sizeof(out))==0);
}

static void invalid_params(void)
{
    const uint8_t duplicate[]={16,0,0,1};
    refusal(duplicate,sizeof(duplicate),2,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t unknown[]={1,0};
    refusal(unknown,sizeof(unknown),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t group[]={34,0};
    refusal(group,sizeof(group),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t forward[]={16,2};
    refusal(forward,sizeof(forward),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t token[]={3,1,1};
    refusal(token,sizeof(token),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_D22_ERR_KVP_FORMAT);
    const uint8_t fill[]={35,4,1,35,1,0};
    refusal(fill,sizeof(fill),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t held[]={33,0};
    refusal(held,sizeof(held),1,MOQ_D22_SCOPE_SUBSCRIBE_TRACKS,8,MOQ_ERR_PROTO);
    const uint8_t held_fill[]={35,1,0};
    refusal(held_fill,sizeof(held_fill),1,MOQ_D22_SCOPE_SUBSCRIBE_TRACKS,8,MOQ_ERR_PROTO);
    const uint8_t update_order[]={34,1};
    refusal(update_order,sizeof(update_order),1,MOQ_D22_SCOPE_UPDATE_SUBSCRIBER,8,MOQ_ERR_PROTO);
    const uint8_t wrong[]={8,0};
    refusal(wrong,sizeof(wrong),1,MOQ_D22_SCOPE_TRACK_STATUS_OK,8,MOQ_ERR_PROTO);
    const uint8_t old_filter[]={33,0,0};
    /* The old zero-length filter leaves a byte; enclosing body must refuse it. */
    moq_buf_reader_t r={old_filter,sizeof(old_filter),0}; moq_d22_params_t out={0};
    CHECK(moq_d22_decode_params(&r,1,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_OK);
    CHECK(r.pos==2);
    const uint8_t huge[]={3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    refusal(huge,sizeof(huge),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
}

static void range_filters(void)
{
    const uint8_t wire[]={37,5,7,3,2,5,5, 0,2,8,9, 1,0};
    moq_buf_reader_t r={wire,sizeof(wire),0}; moq_d22_params_t out={0};
    CHECK(moq_d22_decode_params(&r,3,MOQ_D22_SCOPE_SUBSCRIBE,3,&out)==MOQ_OK);
    CHECK(out.range_count==3);
    refusal(wire,sizeof(wire),3,MOQ_D22_SCOPE_SUBSCRIBE,2,MOQ_D22_ERR_INVALID_FILTER);
    refusal(wire,sizeof(wire),3,MOQ_D22_SCOPE_SUBSCRIBE,0,MOQ_D22_ERR_INVALID_FILTER);
    refusal(wire,sizeof(wire),3,MOQ_D22_SCOPE_UPDATE_PUBLISHER,8,MOQ_ERR_PROTO);
    const uint8_t dupe[]={37,2,7,3,0,2,7,9};
    refusal(dupe,sizeof(dupe),2,MOQ_D22_SCOPE_FETCH,8,MOQ_D22_ERR_INVALID_FILTER);
    const uint8_t priority[]={39,3,0,0x81,0};
    refusal(priority,sizeof(priority),1,MOQ_D22_SCOPE_FETCH,8,MOQ_D22_ERR_INVALID_FILTER);
    const uint8_t odd_property[]={40,3,0,3,0};
    refusal(odd_property,sizeof(odd_property),1,MOQ_D22_SCOPE_FETCH,8,MOQ_D22_ERR_INVALID_FILTER);
    const uint8_t overflow[]={37,11,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1};
    refusal(overflow,sizeof(overflow),1,MOQ_D22_SCOPE_FETCH,8,MOQ_D22_ERR_INVALID_FILTER);
    const uint8_t truncated[]={37,2,0,0x80};
    refusal(truncated,sizeof(truncated),1,MOQ_D22_SCOPE_FETCH,8,MOQ_ERR_PROTO);
    const uint8_t pairs[]={3,2,5,5,7};
    r=(moq_buf_reader_t){pairs,sizeof(pairs),0}; uint64_t prev=0;
    moq_d22_range_t range={0};
    CHECK(moq_d22_range_next(&r,&prev,&range)==MOQ_OK);
    CHECK(range.start==3 && range.end==5 && !range.open_end && prev==5);
    CHECK(moq_d22_range_next(&r,&prev,&range)==MOQ_OK);
    CHECK(range.start==10 && range.end==15 && !range.open_end);
    CHECK(moq_d22_range_next(&r,&prev,&range)==MOQ_OK);
    CHECK(range.start==22 && range.open_end && range.end==UINT64_MAX);
}

static void namespaces(void)
{
    const uint8_t wire[]={2,1,'a',2,'b','c'};
    moq_buf_reader_t r={wire,sizeof(wire),0}; moq_d22_namespace_t ns={0};
    CHECK(moq_d22_decode_namespace(&r,&ns)==MOQ_OK);
    CHECK(ns.count==2 && ns.value_bytes==3 && ns.encoded.data==wire);
    const moq_bytes_t fields[]={{wire+2,1},{wire+4,2}};
    const moq_namespace_t input={fields,2};
    uint8_t encoded[16]; moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_namespace(&w,&input)==MOQ_OK);
    CHECK(w.pos==sizeof(wire) && memcmp(encoded,wire,sizeof(wire))==0);
    for (size_t n=0;n<sizeof(wire);++n) {
        r=(moq_buf_reader_t){wire,n,0}; moq_d22_namespace_t before;
        memset(&ns,0xa5,sizeof(ns)); memcpy(&before,&ns,sizeof(ns));
        CHECK(moq_d22_decode_namespace(&r,&ns)==MOQ_ERR_BUFFER);
        CHECK(r.pos==0 && memcmp(&before,&ns,sizeof(ns))==0);
    }
    const uint8_t empty_field[]={1,0};
    r=(moq_buf_reader_t){empty_field,sizeof(empty_field),0};
    CHECK(moq_d22_decode_namespace(&r,&ns)==MOQ_ERR_PROTO && r.pos==0);
    const uint8_t too_many[]={33}; r=(moq_buf_reader_t){too_many,1,0};
    CHECK(moq_d22_decode_namespace(&r,&ns)==MOQ_ERR_PROTO && r.pos==0);
}

static void scopes_and_values(void)
{
    /* Independently enumerated section 9 allowed sets, including uint8 values
     * above 127 (which discriminate uint8 from vi64). */
    static const struct { uint8_t bytes[12]; size_t len; } params[]={
        {{2,0},2}, {{3,2,3,0},4}, {{4,0},2}, {{6,0},2}, {{8,0},2},
        {{9,0,0},3}, {{10,0},2}, {{16,1},2}, {{32,255},2}, {{33,0},2},
        {{34,2},2}, {{35,1,0},3}, {{37,2,0,0},4}, {{38,2,0,0},4},
        {{39,2,0,0},4}, {{40,3,0,2,0},5}, {{41,3,0,2,0},5},
        {{50,0},2}, {{52,0},2}, {{53,1},2}
    };
    static const struct {
        moq_d22_param_scope_t scope; uint8_t legal[20]; size_t count;
    } scopes[]={
        {MOQ_D22_SCOPE_SUBSCRIBE,{2,3,4,6,16,32,33,34,35,37,38,39,40,50,53},15},
        {MOQ_D22_SCOPE_PUBLISH,{2,3,6,8,9,16,32,33,34},9},
        {MOQ_D22_SCOPE_FETCH,{3,10,32,33,34,37,38,39,40,53},10},
        {MOQ_D22_SCOPE_TRACK_STATUS,{3,53},2},
        {MOQ_D22_SCOPE_PUBLISH_NAMESPACE,{3},1},
        {MOQ_D22_SCOPE_SUBSCRIBE_NAMESPACE,{3},1},
        {MOQ_D22_SCOPE_SUBSCRIBE_TRACKS,{3,16,34,37,38,39,40,41,53},9},
        {MOQ_D22_SCOPE_SUBSCRIBE_OK,{8,9},2},
        {MOQ_D22_SCOPE_PUBLISH_OK,{8},1},
        {MOQ_D22_SCOPE_UPDATE_OK,{8,9},2},
        {MOQ_D22_SCOPE_TRACK_STATUS_OK,{9},1},
        {MOQ_D22_SCOPE_NAMESPACE_OK,{8},1},
        {MOQ_D22_SCOPE_FETCH_OK,{0},0},
        {MOQ_D22_SCOPE_NOTIFY,{9,16,33},3},
        {MOQ_D22_SCOPE_UPDATE_SUBSCRIBER,{2,3,6,16,32,33,35,37,38,39,40,50},12},
        {MOQ_D22_SCOPE_UPDATE_PUBLISHER,{2,3,6,16,32,33,50},7},
        {MOQ_D22_SCOPE_UPDATE_FETCH,{3,32},2},
        {MOQ_D22_SCOPE_UPDATE_PUBLISH_NAMESPACE,{3},1},
        {MOQ_D22_SCOPE_UPDATE_SUBSCRIBE_NAMESPACE,{3,52},2},
        {MOQ_D22_SCOPE_UPDATE_SUBSCRIBE_TRACKS,{3,16,41,52},4},
        {MOQ_D22_SCOPE_FILL,{10,32,33,34,37,38,39,40},8}
    };
    for (size_t s=0;s<sizeof(scopes)/sizeof(scopes[0]);++s) {
        for (size_t i=0;i<sizeof(params)/sizeof(params[0]);++i) {
            bool legal=false;
            for (size_t j=0;j<scopes[s].count;++j)
                if (scopes[s].legal[j]==params[i].bytes[0]) legal=true;
            moq_buf_reader_t r={params[i].bytes,params[i].len,0};
            moq_d22_params_t out={0};
            CHECK(moq_d22_decode_params(&r,1,scopes[s].scope,1,&out)==
                (legal ? MOQ_OK : MOQ_ERR_PROTO));
            CHECK(r.pos==(legal ? r.len : 0));
            r.pos=0; uint64_t previous=0; moq_d22_param_t p={0};
            CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_OK);
            uint8_t encoded[32]={0}; moq_buf_writer_t w={encoded,sizeof(encoded),0};
            CHECK(moq_d22_encode_params(&w,&p,1,scopes[s].scope,1)==
                (legal ? MOQ_OK : MOQ_ERR_INVAL));
            if (legal) CHECK(w.pos==params[i].len &&
                memcmp(encoded,params[i].bytes,params[i].len)==0);
            else CHECK(w.pos==0);
            if (!legal) continue;
            for (size_t n=0;n<params[i].len;++n) {
                r=(moq_buf_reader_t){params[i].bytes,n,0};
                moq_d22_params_t before;
                memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
                CHECK(moq_d22_decode_params(&r,1,scopes[s].scope,1,&out)==MOQ_ERR_BUFFER);
                CHECK(r.pos==0 && memcmp(&before,&out,sizeof(out))==0);
                w=(moq_buf_writer_t){encoded,n,0};
                CHECK(moq_d22_encode_params(&w,&p,1,scopes[s].scope,1)==MOQ_ERR_BUFFER);
                CHECK(w.pos==0);
            }
        }
    }
    const uint8_t maximum[]={2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    moq_buf_reader_t r={maximum,sizeof(maximum),0};
    moq_d22_param_t p={0}; uint64_t previous=0;
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_OK);
    CHECK(p.value.integer==UINT64_MAX);
    uint8_t encoded[32]; moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_OK);
    CHECK(w.pos==sizeof(maximum) && memcmp(encoded,maximum,sizeof(maximum))==0);
    p=(moq_d22_param_t){.type=32,.value.integer=256}; w.pos=0;
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_ERR_INVAL);
    CHECK(w.pos==0);
}

static void tokens_and_fills(void)
{
    const uint8_t tokens[]={3,2,0,7, 0,4,1,8,2,'a', 0,2,2,8, 0,3,3,2,'b'};
    moq_buf_reader_t r={tokens,sizeof(tokens),0}; moq_d22_params_t out={0};
    CHECK(moq_d22_decode_params(&r,4,MOQ_D22_SCOPE_SUBSCRIBE,0,&out)==MOQ_OK);
    CHECK(out.count==4 && out.present==MOQ_D22_PARAM_PRESENT(3));
    r.pos=0; uint64_t previous=0; moq_d22_param_t p[4]={{0}};
    for (size_t i=0;i<4;++i) {
        CHECK(moq_d22_param_next(&r,&previous,&p[i])==MOQ_OK);
        CHECK(p[i].value.token.alias_type==i);
    }
    CHECK(p[1].value.token.alias==8 && p[1].value.token.type==2 &&
        p[1].value.token.value.data[0]=='a');
    uint8_t encoded[64]; moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_params(&w,p,4,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_OK);
    CHECK(w.pos==sizeof(tokens) && memcmp(encoded,tokens,sizeof(tokens))==0);
    const uint8_t bad[][5]={{3,0,0,0,0},{3,1,4,0,0},{3,3,0,1,0},
                           {3,1,3,0,0},{3,2,1,0,0},{35,2,1,33,0}};
    const size_t lens[]={2,3,5,3,4,4};
    for (size_t i=0;i<sizeof(lens)/sizeof(lens[0]);++i)
        refusal(bad[i],lens[i],1,MOQ_D22_SCOPE_SUBSCRIBE,8,
            i==5 ? MOQ_ERR_PROTO : MOQ_D22_ERR_KVP_FORMAT);
    const uint8_t inner_duplicate[]={35,5,2,10,0,0,1};
    refusal(inner_duplicate,sizeof(inner_duplicate),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t separate[]={37,2,0,0, 0,2,1,0};
    r=(moq_buf_reader_t){separate,sizeof(separate),0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_SUBSCRIBE,2,&out)==MOQ_OK);
    const uint8_t independent[]={35,5,1,37,2,0,0, 2,2,0,0};
    r=(moq_buf_reader_t){independent,sizeof(independent),0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_SUBSCRIBE,1,&out)==MOQ_OK);
    const uint8_t distinct_property[]={40,3,1,2,0, 0,3,1,4,0};
    r=(moq_buf_reader_t){distinct_property,sizeof(distinct_property),0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_SUBSCRIBE,2,&out)==MOQ_OK);
}

static void bounds_and_arguments(void)
{
    static uint8_t encoded[65540], field[4097];
    moq_bytes_t part={field,4096}; moq_namespace_t ns={&part,1};
    moq_buf_writer_t w={encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_namespace(&w,&ns)==MOQ_OK);
    moq_buf_reader_t r={encoded,w.pos,0}; moq_d22_namespace_t decoded={0};
    CHECK(moq_d22_decode_namespace(&r,&decoded)==MOQ_OK && decoded.value_bytes==4096);
    part.len=4097; w.pos=0;
    CHECK(moq_d22_encode_namespace(&w,&ns)==MOQ_ERR_INVAL && w.pos==0);
    const uint8_t max_len[]={1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    r=(moq_buf_reader_t){max_len,sizeof(max_len),0};
    CHECK(moq_d22_decode_namespace(&r,&decoded)==MOQ_ERR_PROTO && r.pos==0);
    const uint8_t overflow[]={1,0};
    r=(moq_buf_reader_t){overflow,sizeof(overflow),0};
    uint64_t previous=UINT64_MAX; moq_d22_param_t p,before;
    memset(&p,0xa5,sizeof(p)); memcpy(&before,&p,sizeof(p));
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_ERR_PROTO);
    CHECK(r.pos==0 && previous==UINT64_MAX && memcmp(&before,&p,sizeof(p))==0);
    const uint8_t partial[]={3,0x80};
    r=(moq_buf_reader_t){partial,sizeof(partial),0}; previous=0;
    moq_d22_range_t range,range_before;
    memset(&range,0xa5,sizeof(range)); memcpy(&range_before,&range,sizeof(range));
    CHECK(moq_d22_range_next(&r,&previous,&range)==MOQ_ERR_BUFFER);
    CHECK(r.pos==0 && previous==0 && memcmp(&range,&range_before,sizeof(range))==0);
    const uint8_t start_overflow[]={1};
    r=(moq_buf_reader_t){start_overflow,1,0}; previous=UINT64_MAX;
    CHECK(moq_d22_range_next(&r,&previous,&range)==MOQ_D22_ERR_INVALID_FILTER);
    CHECK(r.pos==0 && previous==UINT64_MAX);
    moq_d22_params_t params={0};
    r=(moq_buf_reader_t){NULL,0,0};
    CHECK(moq_d22_decode_params(&r,UINT64_MAX,MOQ_D22_SCOPE_SUBSCRIBE,0,&params)==MOQ_ERR_BUFFER);
    CHECK(moq_d22_decode_params(&r,0,(moq_d22_param_scope_t)-1,0,&params)==MOQ_ERR_INVAL);
    CHECK(moq_d22_decode_params(NULL,0,MOQ_D22_SCOPE_SUBSCRIBE,0,&params)==MOQ_ERR_INVAL);
    CHECK(moq_d22_decode_fill((moq_bytes_t){NULL,1},0,&params)==MOQ_ERR_INVAL);
    CHECK(moq_d22_encode_params(&w,NULL,SIZE_MAX,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_ERR_INVAL);
    p=(moq_d22_param_t){.type=35,.value.fill={field,SIZE_MAX}};
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_ERR_INVAL);
    CHECK(w.pos==0);
    r=(moq_buf_reader_t){NULL,1,0};
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_ERR_INVAL);
    w=(moq_buf_writer_t){NULL,0,0};
    CHECK(moq_d22_encode_params(&w,NULL,0,MOQ_D22_SCOPE_SUBSCRIBE,0)==MOQ_OK);
    CHECK(moq_d22_decode_fill((moq_bytes_t){NULL,0},0,&params)==MOQ_ERR_PROTO);
    static const uint8_t no_settings[]={0};
    CHECK(moq_d22_decode_fill((moq_bytes_t){no_settings,1},0,&params)==MOQ_OK);
    CHECK(params.count==0 && params.present==0);
    moq_bytes_t fields[32];
    for (size_t i=0;i<32;++i) fields[i]=(moq_bytes_t){field,1};
    ns=(moq_namespace_t){fields,32};
    w=(moq_buf_writer_t){encoded,sizeof(encoded),0};
    CHECK(moq_d22_encode_namespace(&w,&ns)==MOQ_OK);
    r=(moq_buf_reader_t){encoded,w.pos,0};
    CHECK(moq_d22_decode_namespace(&r,&decoded)==MOQ_OK && decoded.count==32);
    /* Namespace UINT64 lengths cannot be narrowed into a size_t. */
    ns=(moq_namespace_t){fields,1}; fields[0].len=SIZE_MAX; w.pos=0;
    CHECK(moq_d22_encode_namespace(&w,&ns)==MOQ_ERR_INVAL && w.pos==0);
}

static void clear_range_keys(void)
{
    const uint8_t clear_then_key[]={37,0, 0,2,7,3};
    const uint8_t key_then_clear[]={37,2,7,3, 0,0};
    const uint8_t duplicate_around_clear[]={37,0, 0,2,7,3, 0,0, 0,2,7,9};
    moq_buf_reader_t r={clear_then_key,sizeof(clear_then_key),0};
    moq_d22_params_t out={0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_UPDATE_SUBSCRIBER,1,&out)==MOQ_OK);
    CHECK(r.pos==r.len && out.count==2 && out.range_count==1);
    r=(moq_buf_reader_t){key_then_clear,sizeof(key_then_clear),0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_UPDATE_SUBSCRIBER,1,&out)==MOQ_OK);
    CHECK(r.pos==r.len && out.count==2 && out.range_count==1);
    refusal(duplicate_around_clear,sizeof(duplicate_around_clear),4,
        MOQ_D22_SCOPE_UPDATE_SUBSCRIBER,2,MOQ_D22_ERR_INVALID_FILTER);
}

static void deferred_filter_refusal(void)
{
    const uint8_t wire[]={39,3,0,0x81,0,14,1};
    for (size_t n=0;n<=sizeof(wire);++n)
        refusal(wire,n,2,MOQ_D22_SCOPE_FETCH,8,
            n==sizeof(wire) ? MOQ_D22_ERR_INVALID_FILTER : MOQ_ERR_BUFFER);
    const uint8_t wrong_scope[]={39,3,0,0x81,0};
    refusal(wrong_scope,sizeof(wrong_scope),1,MOQ_D22_SCOPE_FETCH_OK,8,MOQ_ERR_PROTO);
    const uint8_t invalid_then_unknown[]={39,3,0,0x81,0,5,0};
    refusal(invalid_then_unknown,sizeof(invalid_then_unknown),2,MOQ_D22_SCOPE_FETCH,8,MOQ_ERR_PROTO);
    const uint8_t odd_property_bad_range[]={40,3,0,3,0x80};
    refusal(odd_property_bad_range,sizeof(odd_property_bad_range),1,MOQ_D22_SCOPE_FETCH,8,MOQ_ERR_PROTO);
    const uint8_t overflow_bad_range[]={37,12,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,0x80};
    refusal(overflow_bad_range,sizeof(overflow_bad_range),1,MOQ_D22_SCOPE_FETCH,8,MOQ_ERR_PROTO);
    const uint8_t fills[]={2,39,3,0,0x81,0,5,0};
    moq_d22_params_t out,before;
    memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
    CHECK(moq_d22_decode_fill((moq_bytes_t){fills,sizeof(fills)},8,&out)==MOQ_ERR_PROTO);
    CHECK(memcmp(&out,&before,sizeof(out))==0);
}

static void structural_scan_contract(void)
{
    const uint8_t wire[]={3,3,1,7,0,36,3,0,0x81,0,14,1};
    for (size_t n=0;n<=sizeof(wire);++n) {
        for (unsigned initial=0;initial<2;++initial) {
            moq_buf_reader_t r={wire,n,0};
            moq_d22_params_t out,before;
            memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
            bool invalid=initial!=0;
            moq_result_t rc=moq_d22_scan_params(&r,3,MOQ_D22_SCOPE_FETCH,8,&out,&invalid);
            CHECK(rc==(n==sizeof(wire) ? MOQ_OK : MOQ_ERR_BUFFER));
            if (n<sizeof(wire)) {
                CHECK(r.pos==0 && memcmp(&out,&before,sizeof(out))==0);
                CHECK(invalid==(initial!=0));
            } else {
                if (rc!=MOQ_OK) continue;
                CHECK(invalid && r.pos==sizeof(wire) && out.count==3 && out.range_count==1);
                CHECK(out.encoded.data==wire && out.encoded.len==sizeof(wire));
                moq_buf_reader_t it={out.encoded.data,out.encoded.len,0};
                uint64_t previous=0; moq_d22_param_t token={0};
                CHECK(moq_d22_param_next(&it,&previous,&token)==MOQ_OK);
                CHECK(token.type==3 && token.value.token.alias_type==1 &&
                    token.value.token.alias==7 && token.value.token.type==0);
            }
        }
    }
    const uint8_t bad[]={39,3,0,0x81,0,5,0};
    for (unsigned initial=0;initial<2;++initial) {
        moq_buf_reader_t r={bad,sizeof(bad),0};
        moq_d22_params_t out,before;
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        bool invalid=initial!=0;
        CHECK(moq_d22_scan_params(&r,2,MOQ_D22_SCOPE_FETCH,8,&out,&invalid)==MOQ_ERR_PROTO);
        CHECK(r.pos==0 && invalid==(initial!=0) && memcmp(&out,&before,sizeof(out))==0);
    }
    const uint8_t valid[]={39,2,0,0};
    moq_buf_reader_t r={valid,sizeof(valid),0}; moq_d22_params_t out={0};
    bool invalid=true;
    CHECK(moq_d22_scan_params(&r,1,MOQ_D22_SCOPE_FETCH,8,&out,&invalid)==MOQ_OK);
    CHECK(!invalid && out.count==1 && out.range_count==1);
    CHECK(moq_d22_scan_params(&r,0,MOQ_D22_SCOPE_FETCH,8,&out,NULL)==MOQ_ERR_INVAL);
    /* A strict single-value iterator also validates the whole bounded value
     * before reporting its semantic error, without publishing its cursor. */
    const uint8_t odd_bad[]={40,3,0,3,0x80};
    r=(moq_buf_reader_t){odd_bad,sizeof(odd_bad),0}; uint64_t previous=0;
    moq_d22_param_t p,before;
    memset(&p,0xa5,sizeof(p)); memcpy(&before,&p,sizeof(p));
    CHECK(moq_d22_param_next(&r,&previous,&p)==MOQ_ERR_PROTO);
    CHECK(r.pos==0 && previous==0 && memcmp(&p,&before,sizeof(p))==0);
}

#ifdef MOQ_D22_COUNT_PARAM_READS
static bool clear_read_bound(uint8_t type, size_t count)
{
    static uint8_t wire[UINT16_MAX-1];
    memset(wire,0,sizeof(wire));
    wire[0]=type;
    moq_buf_reader_t r={wire,count*2,0};
    moq_d22_params_t out={0};
    moq_d22_param_scope_t scope=type==41 ? MOQ_D22_SCOPE_UPDATE_SUBSCRIBE_TRACKS :
        MOQ_D22_SCOPE_UPDATE_SUBSCRIBER;
    param_vi64_reads=0;
    CHECK(moq_d22_decode_params(&r,count,scope,1,&out)==MOQ_OK);
    CHECK(r.pos==r.len && out.count==count && out.range_count==0);
    CHECK(out.present==MOQ_D22_PARAM_PRESENT(type));
    CHECK(param_vi64_reads==count*2);
    printf("clear type=%u count=%zu vi64_reads=%zu expected=%zu\n",
        (unsigned)type,count,param_vi64_reads,count*2);
    return param_vi64_reads==count*2;
}

static void clear_range_work_bound(void)
{
    for (uint8_t type=37;type<=41;++type) {
        /* Fail finitely at 64 on the old implementation; do not run its
         * quadratic maximum case after the small discriminating case fails. */
        if (clear_read_bound(type,64))
            CHECK(clear_read_bound(type,UINT16_MAX/2));
    }
}

static bool invalid_property_work_bound(uint8_t type, size_t limit, bool odd_first)
{
    static uint8_t wire[UINT16_MAX];
    moq_buf_writer_t w={wire,sizeof(wire),0};
    const uint8_t set_id=0;
    size_t count=0;
    while (count<limit) {
        uint64_t key=odd_first && count==0 ? 1 : (uint64_t)count*2;
        size_t key_len=moq_vi64_len(key);
        if (moq_buf_writer_remaining(&w)<3+key_len) break;
        CHECK(moq_buf_write_vi64(&w,count==0 ? type : 0)==MOQ_OK);
        CHECK(moq_buf_write_vi64(&w,1+key_len)==MOQ_OK);
        CHECK(moq_buf_write_raw(&w,&set_id,1)==MOQ_OK);
        CHECK(moq_buf_write_vi64(&w,key)==MOQ_OK);
        ++count;
    }
    if (limit==64) CHECK(count==64);
    else CHECK(count==12298 && w.pos==65532);
    moq_d22_param_scope_t scope=type==41 ? MOQ_D22_SCOPE_UPDATE_SUBSCRIBE_TRACKS :
        MOQ_D22_SCOPE_UPDATE_SUBSCRIBER;
    uint64_t max_ranges=odd_first ? 1 : 0;
    moq_buf_reader_t r={wire,w.pos,0};
    moq_d22_params_t out,before;
    memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
    param_vi64_reads=0;
    moq_result_t rc=moq_d22_decode_params(&r,count,scope,max_ranges,&out);
    size_t reads=param_vi64_reads;
    CHECK(rc==MOQ_D22_ERR_INVALID_FILTER);
    CHECK(r.pos==0 && memcmp(&out,&before,sizeof(out))==0);
    CHECK(reads==3*count);
    printf("invalid properties type=%u count=%zu bytes=%zu max=%llu odd_first=%u vi64_reads=%zu expected=%zu\n",
        (unsigned)type,count,w.pos,(unsigned long long)max_ranges,
        (unsigned)odd_first,reads,3*count);

    /* The valid positive-limit case still scans distinct keys. Inventory the
     * finite small case without locking in quadratic work as a test oracle. */
    if (limit==64 && !odd_first) {
        param_vi64_reads=0;
        CHECK(moq_d22_decode_params(&r,count,scope,1,&out)==MOQ_OK);
        CHECK(r.pos==w.pos && out.count==count && out.range_count==0);
        printf("positive-limit inventory type=%u count=%zu vi64_reads=%zu\n",
            (unsigned)type,count,param_vi64_reads);
    }

    CHECK(w.pos+2<=sizeof(wire));
    wire[w.pos]=(uint8_t)(44-type); wire[w.pos+1]=0;
    param_vi64_reads=0;
    refusal(wire,w.pos+2,count+1,scope,max_ranges,MOQ_ERR_PROTO);
    CHECK(param_vi64_reads==3*count+1);
    /* A legal next parameter with its vi64 value missing remains incomplete. */
    wire[w.pos]=(uint8_t)((type==41 ? 52 : 50)-type);
    param_vi64_reads=0;
    refusal(wire,w.pos+1,count+1,scope,max_ranges,MOQ_ERR_BUFFER);
    CHECK(param_vi64_reads==3*count+2);
    return rc==MOQ_D22_ERR_INVALID_FILTER && reads==3*count;
}

static void invalid_property_sequence_work_bound(void)
{
    for (uint8_t type=40;type<=41;++type) {
        /* Keep a failure bounded: the maximum runs only after the small case passes. */
        if (invalid_property_work_bound(type,64,false))
            CHECK(invalid_property_work_bound(type,UINT16_MAX,false));
        CHECK(invalid_property_work_bound(type,64,true));
    }
}
#endif

/* Section 9.20.15: the FILL_PARAMETERS value is Parameters "encoded as if
 * they were Parameters for a separate message", i.e. a Number of Parameters
 * then that many, inside the parameter's Length (the selected reading,
 * supported by the moq-wg/moq-transport#1991 clarification). The count must
 * account for every
 * byte of the value, in both directions, and is never inferred from the
 * Length. */
static void fill_parameter_count(void)
{
    moq_d22_params_t out,before;
    /* A fill with no settings of its own: count 0. */
    const uint8_t empty[]={35,1,0};
    moq_buf_reader_t r={empty,sizeof(empty),0};
    CHECK(moq_d22_decode_params(&r,1,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_OK);
    CHECK(r.pos==sizeof(empty) && out.count==1);
    /* An empty value carries no count. */
    const uint8_t no_count[]={35,0};
    refusal(no_count,sizeof(no_count),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* The count's parameters must end exactly at the Length. */
    const uint8_t short_count[]={35,5,1,10,0,22,1};
    refusal(short_count,sizeof(short_count),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t long_count[]={35,3,2,10,0};
    refusal(long_count,sizeof(long_count),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t zero_with_bytes[]={35,3,0,10,0};
    refusal(zero_with_bytes,sizeof(zero_with_bytes),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* The largest vi64 count with nothing after it. */
    const uint8_t huge_count[]={35,9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    refusal(huge_count,sizeof(huge_count),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* A count whose own encoding is cut by the Length. */
    const uint8_t cut_count[]={35,1,0x80,0x01};
    refusal(cut_count,sizeof(cut_count),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* Inside the value: a duplicate, a parameter outside its scope (FORWARD,
     * AUTHORIZATION_TOKEN, a nested FILL_PARAMETERS) and a type past 63. */
    const uint8_t inner_dup[]={35,5,2,32,1,0,2};
    refusal(inner_dup,sizeof(inner_dup),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t inner_forward[]={35,3,1,16,1};
    refusal(inner_forward,sizeof(inner_forward),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t inner_token[]={35,5,1,3,2,3,0};
    refusal(inner_token,sizeof(inner_token),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t inner_fill[]={35,4,1,35,1,0};
    refusal(inner_fill,sizeof(inner_fill),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    const uint8_t inner_wide[]={35,3,1,0x40,0};
    refusal(inner_wide,sizeof(inner_wide),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* The nested delta chain restarts at 0: after the outer GROUP_ORDER
     * (0x22) and FILL_PARAMETERS (0x23), the first nested delta 0x21 is
     * LOCATION_FILTER itself, then +1 is GROUP_ORDER (0x22) again. */
    const uint8_t reset[]={34,1, 1,5,2,33,5,1,2};
    r=(moq_buf_reader_t){reset,sizeof(reset),0};
    CHECK(moq_d22_decode_params(&r,2,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_OK);
    CHECK(r.pos==sizeof(reset) && out.count==2);
    CHECK(moq_d22_decode_fill((moq_bytes_t){reset+4,5},8,&out)==MOQ_OK);
    CHECK(out.count==2 && out.present==
          (MOQ_D22_PARAM_PRESENT(33)|MOQ_D22_PARAM_PRESENT(34)));
    {
        moq_buf_reader_t it={out.encoded.data,out.encoded.len,0};
        uint64_t prev=0; moq_d22_param_t ip={0};
        CHECK(moq_d22_param_next(&it,&prev,&ip)==MOQ_OK);
        CHECK(ip.type==33 && ip.value.filter.type==MOQ_D22_FILTER_NEXT_OBJECT);
        CHECK(moq_d22_param_next(&it,&prev,&ip)==MOQ_OK);
        CHECK(ip.type==34 && ip.value.integer==2 && it.pos==it.len);
    }
    /* A sender continuing the outer chain would write SUBGROUP_FILTER
     * (0x25) as delta 2 from 0x23; restarted, that delta is type 0x02,
     * outside the fill's scope. */
    const uint8_t continued[]={35,5,1,2,2,0,0};
    refusal(continued,sizeof(continued),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* A value that leaves the count out: its first nested type (0x0A) reads
     * as the count, and `80 fa` then as Type Delta 250, outside the scope. */
    const uint8_t uncounted[]={35,6,10,0x80,0xfa,23,1,1};
    refusal(uncounted,sizeof(uncounted),1,MOQ_D22_SCOPE_SUBSCRIBE,8,MOQ_ERR_PROTO);
    /* A value valid both with and without a count decodes only as counted:
     * ten parameters, not FILL_TIMEOUT plus one Subgroup Filter. */
    static const uint8_t both[]={10,10,27,22,0,1,1,7,1,1,3,1,0,0,1,1,1,1,0,0,
                                 1,1,1,0,1,0};
    memset(&out,0xa5,sizeof(out));
    CHECK(moq_d22_decode_fill((moq_bytes_t){both,sizeof(both)},8,&out)==MOQ_OK);
    CHECK(out.count==10 && out.encoded.data==both+1 &&
          out.encoded.len==sizeof(both)-1);
    /* The peer SUBSCRIBE parameter block that carries FILL_PARAMETERS
     * {count 1, LOCATION_FILTER Relative Start, StartGroup 1}. */
    static const uint8_t peer[]={16,1, 16,0xff, 1,5, 1,2, 1,4,1,33,1,1};
    r=(moq_buf_reader_t){peer,sizeof(peer),0};
    CHECK(moq_d22_decode_params(&r,5,MOQ_D22_SCOPE_SUBSCRIBE,8,&out)==MOQ_OK);
    CHECK(r.pos==sizeof(peer) && out.count==5);
    /* Truncating the value is malformed, never a shorter fill. */
    for (size_t n=0;n<4;++n) {
        memset(&out,0xa5,sizeof(out)); memcpy(&before,&out,sizeof(out));
        CHECK(moq_d22_decode_fill((moq_bytes_t){peer+10,n},8,&out)==MOQ_ERR_PROTO);
        CHECK(memcmp(&out,&before,sizeof(out))==0);
    }
    memset(&out,0xa5,sizeof(out));
    CHECK(moq_d22_decode_fill((moq_bytes_t){peer+10,4},8,&out)==MOQ_OK);
    CHECK(out.count==1 && out.present==MOQ_D22_PARAM_PRESENT(33));
    /* Encoding refuses an uncounted value and writes the counted one. */
    const uint8_t bare[]={10,0};
    moq_d22_param_t p={.type=35,.value.fill={bare,sizeof(bare)}};
    uint8_t enc[16]; moq_buf_writer_t w={enc,sizeof(enc),0};
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_ERR_INVAL);
    CHECK(w.pos==0);
    const uint8_t counted[]={1,10,0};
    p.value.fill=(moq_bytes_t){counted,sizeof(counted)};
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_OK);
    CHECK(w.pos==5 && enc[0]==35 && enc[1]==3 && memcmp(enc+2,counted,3)==0);
    /* Capacity: every writer too small for Type, Length, count and the
     * nested parameter commits nothing, including one byte short of the
     * count and of its last byte. */
    for (size_t cap=0;cap<5;++cap) {
        memset(enc,0xa5,sizeof(enc));
        w=(moq_buf_writer_t){enc,cap,0};
        CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_ERR_BUFFER);
        CHECK(w.pos==0);
        bool untouched=true;
        for (size_t i=cap;i<sizeof(enc);++i) untouched&=enc[i]==0xa5;
        CHECK(untouched);
    }
    /* A count that disagrees with the entries is refused on encode too. */
    const uint8_t miscounted[]={2,10,0};
    p.value.fill=(moq_bytes_t){miscounted,sizeof(miscounted)};
    w=(moq_buf_writer_t){enc,sizeof(enc),0};
    CHECK(moq_d22_encode_params(&w,&p,1,MOQ_D22_SCOPE_SUBSCRIBE,8)==MOQ_ERR_INVAL);
    CHECK(w.pos==0);
}

int main(void)
{
    literal_params(); invalid_params(); range_filters(); namespaces();
    scopes_and_values(); tokens_and_fills(); bounds_and_arguments();
    clear_range_keys();
    deferred_filter_refusal();
    structural_scan_contract();
#ifdef MOQ_D22_COUNT_PARAM_READS
    clear_range_work_bound();
    invalid_property_sequence_work_bound();
#endif
    fill_parameter_count();
    printf("draft-22 parameters: %d failures\n",failures);
    return failures ? 1 : 0;
}
