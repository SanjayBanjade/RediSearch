#include <string.h>
#include <assert.h>

#include "document.h"
#include "forward_index.h"
#include "numeric_filter.h"
#include "numeric_index.h"
#include "rmutil/strings.h"
#include "rmutil/util.h"
#include "util/mempool.h"
#include "spec.h"
#include "tokenize.h"
#include "util/logging.h"
#include "rmalloc.h"
#include "indexer.h"
#include "tag_index.h"
#include "aggregate/expr/expression.h"

// Memory pool for RSAddDocumentContext contexts
static mempool_t *actxPool_g = NULL;

// For documentation, see these functions' definitions
static void *allocDocumentContext(void) {
  // See if there's one in the pool?
  RSAddDocumentCtx *aCtx = calloc(1, sizeof(*aCtx));
  return aCtx;
}

static void freeDocumentContext(void *p) {
  RSAddDocumentCtx *aCtx = p;
  if (aCtx->fwIdx) {
    ForwardIndexFree(aCtx->fwIdx);
  }

  free(aCtx->fspecs);
  free(aCtx->fdatas);
  free(aCtx);
}

#define DUP_FIELD_ERRSTR "Requested to index field twice"

static int AddDocumentCtx_SetDocument(RSAddDocumentCtx *aCtx, IndexSpec *sp, Document *base,
                                      size_t oldFieldCount) {
  aCtx->doc = *base;
  Document *doc = &aCtx->doc;

  if (oldFieldCount < doc->numFields) {
    // Pre-allocate the field specs
    aCtx->fspecs = realloc(aCtx->fspecs, sizeof(*aCtx->fspecs) * doc->numFields);
    aCtx->fdatas = realloc(aCtx->fdatas, sizeof(*aCtx->fdatas) * doc->numFields);
  }

  size_t numIndexable = 0;

  // size: uint16_t * SPEC_MAX_FIELDS
  FieldSpecDedupeArray dedupe = {0};
  int hasTextFields = 0;
  int hasOtherFields = 0;

  for (int i = 0; i < doc->numFields; i++) {
    const DocumentField *f = doc->fields + i;
    const FieldSpec *fs = IndexSpec_GetField(sp, f->name, strlen(f->name));
    if (fs && f->text) {

      aCtx->fspecs[i] = *fs;
      if (dedupe[fs->index]) {
        QueryError_SetErrorFmt(&aCtx->status, QUERY_EDUPFIELD, "Tried to insert `%s` twice",
                               fs->name);
        return -1;
      }

      dedupe[fs->index] = 1;

      if (FieldSpec_IsSortable(fs)) {
        // mark sortable fields to be updated in the state flags
        aCtx->stateFlags |= ACTX_F_SORTABLES;
      }

      if (FieldSpec_IsIndexable(fs)) {
        if (fs->type == FIELD_FULLTEXT) {
          numIndexable++;
          hasTextFields = 1;
        } else {
          hasOtherFields = 1;
        }
      }
    } else {
      // Field is not in schema, or is duplicate
      aCtx->fspecs[i].name = NULL;
    }
  }

  if (hasTextFields || hasOtherFields) {
    aCtx->stateFlags |= ACTX_F_INDEXABLES;
  }
  if (!hasTextFields) {
    aCtx->stateFlags |= ACTX_F_TEXTINDEXED;
  }
  if (!hasOtherFields) {
    aCtx->stateFlags |= ACTX_F_OTHERINDEXED;
  }

  if ((aCtx->stateFlags & ACTX_F_SORTABLES) && aCtx->sv == NULL) {
    aCtx->sv = NewSortingVector(sp->sortables->len);
  }

  if ((aCtx->options & DOCUMENT_ADD_NOSAVE) == 0 && numIndexable &&
      (sp->flags & Index_StoreByteOffsets)) {
    if (!aCtx->byteOffsets) {
      aCtx->byteOffsets = NewByteOffsets();
      ByteOffsetWriter_Init(&aCtx->offsetsWriter);
    }
    RSByteOffsets_ReserveFields(aCtx->byteOffsets, numIndexable);
  }
  return 0;
}

RSAddDocumentCtx *NewAddDocumentCtx(IndexSpec *sp, Document *b, QueryError *status) {

  if (!actxPool_g) {
    actxPool_g = mempool_new(16, allocDocumentContext, freeDocumentContext);
  }

  // Get a new context
  RSAddDocumentCtx *aCtx = mempool_get(actxPool_g);
  aCtx->stateFlags = 0;
  QueryError_ClearError(&aCtx->status);
  aCtx->totalTokens = 0;
  aCtx->client.bc = NULL;
  aCtx->next = NULL;
  aCtx->specFlags = sp->flags;
  int indexerOptions = 0;
  if (sp->flags & Index_Temporary) {
    indexerOptions = INDEXER_THREADLESS;
  }
  aCtx->indexer = GetDocumentIndexer(sp->name, indexerOptions);

  // Assign the document:
  if (AddDocumentCtx_SetDocument(aCtx, sp, b, aCtx->doc.numFields) != 0) {
    *status = aCtx->status;
    aCtx->status.detail = NULL;
    mempool_release(actxPool_g, aCtx);
    return NULL;
  }

  // try to reuse the forward index on recycled contexts
  if (aCtx->fwIdx) {
    ForwardIndex_Reset(aCtx->fwIdx, &aCtx->doc, sp->flags);
  } else {
    aCtx->fwIdx = NewForwardIndex(&aCtx->doc, sp->flags);
  }

  if (sp->smap) {
    // we get a read only copy of the synonym map for accessing in the index thread with out worring
    // about thready safe issues
    aCtx->fwIdx->smap = SynonymMap_GetReadOnlyCopy(sp->smap);
  } else {
    aCtx->fwIdx->smap = NULL;
  }

  aCtx->tokenizer = GetTokenizer(b->language, aCtx->fwIdx->stemmer, sp->stopwords);
  StopWordList_Ref(sp->stopwords);

  aCtx->doc.docId = 0;
  return aCtx;
}

static void doReplyFinish(RSAddDocumentCtx *aCtx, RedisModuleCtx *ctx) {
  if(aCtx->donecb){
    aCtx->donecb(aCtx, ctx, NULL);
  }
  AddDocumentCtx_Free(aCtx);
}

static int replyCallback(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  RSAddDocumentCtx *aCtx = RedisModule_GetBlockedClientPrivateData(ctx);
  doReplyFinish(aCtx, ctx);
  return REDISMODULE_OK;
}

static void threadCallback(void *p) {
  Document_AddToIndexes(p);
}

void AddDocumentCtx_Finish(RSAddDocumentCtx *aCtx) {
  if (aCtx->stateFlags & ACTX_F_NOBLOCK) {
    doReplyFinish(aCtx, aCtx->client.sctx->redisCtx);
  } else {
    RedisModule_UnblockClient(aCtx->client.bc, aCtx);
  }
}

// How many bytes in a document to warrant it being tokenized in a separate thread
#define SELF_EXEC_THRESHOLD 1024

void Document_Dump(const Document *doc) {
  printf("Document Key: %s. ID=%llu\n", RedisModule_StringPtrLen(doc->docKey, NULL), doc->docId);
  for (size_t ii = 0; ii < doc->numFields; ++ii) {
    printf("  [%lu]: %s => %s\n", ii, doc->fields[ii].name,
           RedisModule_StringPtrLen(doc->fields[ii].text, NULL));
  }
}

static void AddDocumentCtx_UpdateNoIndex(RSAddDocumentCtx *aCtx, RedisSearchCtx *sctx);

static int AddDocumentCtx_ReplaceMerge(RSAddDocumentCtx *aCtx, RedisSearchCtx *sctx) {
  /**
   * The REPLACE operation contains fields which must be reindexed. This means
   * that a new document ID needs to be assigned, and as a consequence, all
   * fields must be reindexed.
   */
  // Free the old field data
  size_t oldFieldCount = aCtx->doc.numFields;
  Document_ClearDetachedFields(&aCtx->doc, sctx->redisCtx);

  // Get a list of fields needed to be loaded for reindexing
  const char **toLoad = array_new(const char *, 8);

  for (size_t ii = 0; ii < sctx->spec->numFields; ++ii) {
    // TODO: We should only need to reload fields that are actually being reindexed!
    toLoad = array_append(toLoad, sctx->spec->fields[ii].name);
  }

  // for (size_t ii = 0; ii < array_len(toLoad); ++ii) {
  //   printf("Loading: %s\n", toLoad[ii]);
  // }

  int rv =
      Redis_LoadDocumentEx(sctx, aCtx->doc.docKey, toLoad, array_len(toLoad), &aCtx->doc, NULL);
  // int rv = Redis_LoadDocument(sctx, aCtx->doc.docKey, &aCtx->doc);
  array_free(toLoad);

  if (rv != REDISMODULE_OK) {
    QueryError_SetError(&aCtx->status, QUERY_ENODOC, "Could not load existing document");
    aCtx->donecb(aCtx, sctx->redisCtx, NULL);
    AddDocumentCtx_Free(aCtx);
    return 1;
  }

  // Keep hold of the new fields.
  Document_DetachFields(&aCtx->doc, sctx->redisCtx);
  AddDocumentCtx_SetDocument(aCtx, sctx->spec, &aCtx->doc, oldFieldCount);

  return 0;
}

static int handlePartialUpdate(RSAddDocumentCtx *aCtx, RedisSearchCtx *sctx) {
  // Handle partial update of fields
  if (aCtx->stateFlags & ACTX_F_INDEXABLES) {
    return AddDocumentCtx_ReplaceMerge(aCtx, sctx);
  } else {
    // No indexable fields are updated, we can just update the metadata.
    // Quick update just updates the score, payload and sortable fields of the document.
    // Thus full-reindexing of the document is not required
    AddDocumentCtx_UpdateNoIndex(aCtx, sctx);
    return 1;
  }
}

void AddDocumentCtx_Submit(RSAddDocumentCtx *aCtx, RedisSearchCtx *sctx, uint32_t options) {
  aCtx->options = options;
  if ((aCtx->options & DOCUMENT_ADD_PARTIAL) && handlePartialUpdate(aCtx, sctx)) {
    return;
  }
  if (AddDocumentCtx_IsBlockable(aCtx)) {
    aCtx->client.bc = RedisModule_BlockClient(sctx->redisCtx, replyCallback, NULL, NULL, 0);
  } else {
    aCtx->client.sctx = sctx;
  }
  assert(aCtx->client.bc);
  size_t totalSize = 0;
  for (size_t ii = 0; ii < aCtx->doc.numFields; ++ii) {
    const FieldSpec *fs = aCtx->fspecs + ii;
    if (fs->name && (fs->type == FIELD_FULLTEXT || fs->type == FIELD_TAG)) {
      size_t n;
      RedisModule_StringPtrLen(aCtx->doc.fields[ii].text, &n);
      totalSize += n;
    }
  }

  if (totalSize >= SELF_EXEC_THRESHOLD && AddDocumentCtx_IsBlockable(aCtx)) {
    ConcurrentSearch_ThreadPoolRun(threadCallback, aCtx, CONCURRENT_POOL_INDEX);
  } else {
    Document_AddToIndexes(aCtx);
  }
}

void AddDocumentCtx_Free(RSAddDocumentCtx *aCtx) {
  // Destroy the common fields:
  Document_FreeDetached(&aCtx->doc, aCtx->indexer->redisCtx);

  if (aCtx->sv) {
    SortingVector_Free(aCtx->sv);
    aCtx->sv = NULL;
  }

  if (aCtx->byteOffsets) {
    RSByteOffsets_Free(aCtx->byteOffsets);
    aCtx->byteOffsets = NULL;
  }

  if (aCtx->tokenizer) {
    // aCtx->tokenizer->Free(aCtx->tokenizer);
    Tokenizer_Release(aCtx->tokenizer);
    aCtx->tokenizer = NULL;
  }

  if (aCtx->oldMd) {
    DMD_Decref(aCtx->oldMd);
    aCtx->oldMd = NULL;
  }

  ByteOffsetWriter_Cleanup(&aCtx->offsetsWriter);
  QueryError_ClearError(&aCtx->status);

  mempool_release(actxPool_g, aCtx);
}

#define FIELD_HANDLER(name)                                                                \
  static int name(RSAddDocumentCtx *aCtx, const DocumentField *field, const FieldSpec *fs, \
                  fieldData *fdata, QueryError *status)

#define FIELD_BULK_INDEXER(name)                                                    \
  static int name(IndexBulkData *bulk, RSAddDocumentCtx *aCtx, RedisSearchCtx *ctx, \
                  DocumentField *field, const FieldSpec *fs, fieldData *fdata, QueryError *status)

#define FIELD_BULK_CTOR(name) \
  static void name(IndexBulkData *bulk, const FieldSpec *fs, RedisSearchCtx *ctx)
#define FIELD_BULK_FINALIZER(name) static void name(IndexBulkData *bulk, RedisSearchCtx *ctx)

#define FIELD_PREPROCESSOR FIELD_HANDLER

FIELD_PREPROCESSOR(fulltextPreprocessor) {
  size_t fl;
  const char *c = RedisModule_StringPtrLen(field->text, &fl);
  if (FieldSpec_IsSortable(fs)) {
    RSSortingVector_Put(aCtx->sv, fs->sortIdx, (void *)c, RS_SORTABLE_STR);
  }

  if (FieldSpec_IsIndexable(fs)) {
    ForwardIndexTokenizerCtx tokCtx;
    VarintVectorWriter *curOffsetWriter = NULL;
    RSByteOffsetField *curOffsetField = NULL;
    if (aCtx->byteOffsets) {
      curOffsetField =
          RSByteOffsets_AddField(aCtx->byteOffsets, fs->textOpts.id, aCtx->totalTokens + 1);
      curOffsetWriter = &aCtx->offsetsWriter;
    }

    ForwardIndexTokenizerCtx_Init(&tokCtx, aCtx->fwIdx, c, curOffsetWriter, fs->textOpts.id,
                                  fs->textOpts.weight);

    uint32_t options = TOKENIZE_DEFAULT_OPTIONS;
    if (FieldSpec_IsNoStem(fs)) {
      options |= TOKENIZE_NOSTEM;
    }
    if (FieldSpec_IsPhonetics(fs)) {
      options |= TOKENIZE_PHONETICS;
    }
    aCtx->tokenizer->Start(aCtx->tokenizer, (char *)c, fl, options);

    Token tok;
    uint32_t lastTokPos = 0;
    uint32_t newTokPos;
    while (0 != (newTokPos = aCtx->tokenizer->Next(aCtx->tokenizer, &tok))) {
      forwardIndexTokenFunc(&tokCtx, &tok);
      lastTokPos = newTokPos;
    }

    if (curOffsetField) {
      curOffsetField->lastTokPos = lastTokPos;
    }
    aCtx->totalTokens = lastTokPos;
  }
  return 0;
}

FIELD_PREPROCESSOR(numericPreprocessor) {
  if (RedisModule_StringToDouble(field->text, &fdata->numeric) == REDISMODULE_ERR) {
    QueryError_SetCode(status, QUERY_EPARSEARGS);
    return -1;
  }

  // If this is a sortable numeric value - copy the value to the sorting vector
  if (FieldSpec_IsSortable(fs)) {
    RSSortingVector_Put(aCtx->sv, fs->sortIdx, &fdata->numeric, RS_SORTABLE_NUM);
  }
  return 0;
}

FIELD_BULK_INDEXER(numericIndexer) {
  NumericRangeTree *rt = bulk->indexData;
  // NumericRangeTree *rt = OpenNumericIndex(ctx, fs->name, &idxKey);
  NumericRangeTree_Add(rt, aCtx->doc.docId, fdata->numeric);
  // RedisModule_CloseKey(idxKey);
  return 0;
}

FIELD_BULK_CTOR(numericCtor) {
  RedisModuleString *keyName = IndexSpec_GetFormattedKey(ctx->spec, fs);
  bulk->indexData = OpenNumericIndex(ctx, keyName, &bulk->indexKey);
}

FIELD_PREPROCESSOR(geoPreprocessor) {
  const char *c = RedisModule_StringPtrLen(field->text, NULL);
  char *pos = strpbrk(c, " ,");
  if (!pos) {
    QueryError_SetCode(status, QUERY_EGEOFORMAT);
    return -1;
  }
  *pos = '\0';
  pos++;
  fdata->geo.slon = (char *)c;
  fdata->geo.slat = (char *)pos;
  return 0;
}

FIELD_BULK_INDEXER(geoIndexer) {
  GeoIndex gi = {.ctx = ctx, .sp = fs};
  int rv = GeoIndex_AddStrings(&gi, aCtx->doc.docId, fdata->geo.slon, fdata->geo.slat);

  if (rv == REDISMODULE_ERR) {
    QueryError_SetError(status, QUERY_EGENERIC, "Could not index geo value");
    return -1;
  }
  return 0;
}

FIELD_PREPROCESSOR(tagPreprocessor) {

  fdata->tags = TagIndex_Preprocess(&fs->tagOpts, field);

  if (fdata->tags == NULL) {
    return 0;
  }
  if (FieldSpec_IsSortable(fs)) {
    size_t fl;
    const char *c = RedisModule_StringPtrLen(field->text, &fl);
    RSSortingVector_Put(aCtx->sv, fs->sortIdx, (void *)c, RS_SORTABLE_STR);
  }
  return 0;
}

FIELD_BULK_CTOR(tagCtor) {
  RedisModuleString *kname = IndexSpec_GetFormattedKey(ctx->spec, fs);
  bulk->indexData = TagIndex_Open(ctx, kname, 1, &bulk->indexKey);
}

FIELD_BULK_INDEXER(tagIndexer) {
  int rc = 0;
  if (!bulk->indexData) {
    QueryError_SetError(status, QUERY_EGENERIC, "Could not open tag index for indexing");
    rc = -1;
  } else {
    TagIndex_Index(bulk->indexData, (const char **)fdata->tags, array_len(fdata->tags),
                   aCtx->doc.docId);
  }
  if (fdata->tags) {
    TagIndex_FreePreprocessedData(fdata->tags);
  }
  return rc;
}

PreprocessorFunc GetIndexPreprocessor(const FieldType ft) {
  switch (ft) {
    case FIELD_FULLTEXT:
      return fulltextPreprocessor;
    case FIELD_NUMERIC:
      return numericPreprocessor;
    case FIELD_GEO:
      return geoPreprocessor;
    case FIELD_TAG:
      return tagPreprocessor;
    default:
      return NULL;
  }
}

static BulkIndexer geoBulkProcs = {.BulkAdd = geoIndexer};
static BulkIndexer numBulkProcs = {.BulkInit = numericCtor, .BulkAdd = numericIndexer};
static BulkIndexer tagBulkProcs = {.BulkInit = tagCtor, .BulkAdd = tagIndexer};

const BulkIndexer *GetBulkIndexer(const FieldType ft) {
  switch (ft) {
    case FIELD_NUMERIC:
      return &numBulkProcs;
    case FIELD_TAG:
      return &tagBulkProcs;
    case FIELD_GEO:
      return &geoBulkProcs;
    default:
      abort();
      return NULL;
  }
}

int Document_AddToIndexes(RSAddDocumentCtx *aCtx) {
  Document *doc = &aCtx->doc;
  int ourRv = REDISMODULE_OK;

  for (int i = 0; i < doc->numFields; i++) {
    const FieldSpec *fs = aCtx->fspecs + i;
    fieldData *fdata = aCtx->fdatas + i;
    if (fs->name == NULL) {
      LG_DEBUG("Skipping field %s not in index!", doc->fields[i].name);
      continue;
    }

    // Get handler
    PreprocessorFunc pp = GetIndexPreprocessor(fs->type);
    if (pp == NULL) {
      continue;
    }

    if (pp(aCtx, &doc->fields[i], fs, fdata, &aCtx->status) != 0) {
      ourRv = REDISMODULE_ERR;
      goto cleanup;
    }
  }

  if (Indexer_Add(aCtx->indexer, aCtx) != 0) {
    ourRv = REDISMODULE_ERR;
    goto cleanup;
  }

cleanup:
  if (ourRv != REDISMODULE_OK) {
    QueryError_SetCode(&aCtx->status, QUERY_EGENERIC);
    AddDocumentCtx_Finish(aCtx);
  }
  return ourRv;
}

/* Evaluate an IF expression (e.g. IF "@foo == 'bar'") against a document, by getting the properties
 * from the sorting table or from the hash representation of the document.
 *
 * NOTE: This is disconnected from the document indexing flow, and loads the document and discards
 * of it internally
 *
 * Returns  REDISMODULE_ERR on failure, OK otherwise*/
int Document_EvalExpression(RedisSearchCtx *sctx, RedisModuleString *key, const char *expr,
                            int *result, QueryError *status) {

  int rc = REDISMODULE_ERR;
  const RSDocumentMetadata *dmd = DocTable_GetByKeyR(&sctx->spec->docs, key);
  if (!dmd) {
    // We don't know the document...
    QueryError_SetError(status, QUERY_ENODOC, "");
    return REDISMODULE_ERR;
  }

  // Try to parser the expression first, fail if we can't
  RSExpr *e = ExprAST_Parse(expr, strlen(expr), status);
  if (!e) {
    return REDISMODULE_ERR;
  }

  RLookup lookup_s;
  RLookupRow row = {0};
  IndexSpecCache *spcache = IndexSpec_GetSpecCache(sctx->spec);
  RLookup_Init(&lookup_s, spcache);
  if (ExprAST_GetLookupKeys(e, &lookup_s, status) == EXPR_EVAL_ERR) {
    goto done;
  }

  RLookupLoadOptions loadopts = {.sctx = sctx, .dmd = dmd, .status = status};
  if (RLookup_LoadDocument(&lookup_s, &row, &loadopts) != REDISMODULE_OK) {
    // printf("Couldn't load document!\n");
    goto done;
  }

  ExprEval evaluator = {.err = status, .lookup = &lookup_s, .res = NULL, .srcrow = &row, .root = e};
  RSValue rv = RSVALUE_STATIC;
  if (ExprEval_Eval(&evaluator, &rv) != EXPR_EVAL_OK) {
    // printf("Eval not OK!!! SAD!!\n");
    goto done;
  }

  *result = RSValue_BoolTest(&rv);
  RSValue_Clear(&rv);
  rc = REDISMODULE_OK;

// Clean up:
done:
  if (e) {
    ExprAST_Free(e);
  }
  RLookupRow_Cleanup(&row);
  RLookup_Cleanup(&lookup_s);
  return rc;
}

static void AddDocumentCtx_UpdateNoIndex(RSAddDocumentCtx *aCtx, RedisSearchCtx *sctx) {
#define BAIL(s)                                            \
  do {                                                     \
    QueryError_SetError(&aCtx->status, QUERY_EGENERIC, s); \
    goto done;                                             \
  } while (0);

  Document *doc = &aCtx->doc;
  t_docId docId = DocTable_GetIdR(&sctx->spec->docs, doc->docKey);
  if (docId == 0) {
    BAIL("Couldn't load old document");
  }
  RSDocumentMetadata *md = DocTable_Get(&sctx->spec->docs, docId);
  if (!md) {
    BAIL("Couldn't load document metadata");
  }

  // Update the score
  md->score = doc->score;
  // Set the payload if needed
  if (doc->payload) {
    DocTable_SetPayload(&sctx->spec->docs, docId, doc->payload, doc->payloadSize);
  }

  if (aCtx->stateFlags & ACTX_F_SORTABLES) {
    FieldSpecDedupeArray dedupes = {0};
    // Update sortables if needed
    for (int i = 0; i < doc->numFields; i++) {
      DocumentField *f = &doc->fields[i];
      const FieldSpec *fs = IndexSpec_GetField(sctx->spec, f->name, strlen(f->name));
      if (fs == NULL || !FieldSpec_IsSortable(fs)) {
        continue;
      }

      if (dedupes[fs->index]) {
        BAIL(DUP_FIELD_ERRSTR);
      }

      dedupes[fs->index] = 1;

      int idx = IndexSpec_GetFieldSortingIndex(sctx->spec, f->name, strlen(f->name));
      if (idx < 0) continue;

      if (!md->sortVector) {
        md->sortVector = NewSortingVector(sctx->spec->sortables->len);
      }

      switch (fs->type) {
        case FIELD_FULLTEXT:
          RSSortingVector_Put(md->sortVector, idx, (void *)RedisModule_StringPtrLen(f->text, NULL),
                              RS_SORTABLE_STR);
          break;
        case FIELD_NUMERIC: {
          double numval;
          if (RedisModule_StringToDouble(f->text, &numval) == REDISMODULE_ERR) {
            BAIL("Could not parse numeric index value");
          }
          RSSortingVector_Put(md->sortVector, idx, &numval, RS_SORTABLE_NUM);
          break;
        }
        default:
          BAIL("Unsupported sortable type");
          break;
      }
    }
  }

done:
  aCtx->donecb(aCtx, sctx->redisCtx, NULL);
  AddDocumentCtx_Free(aCtx);
}

DocumentField *Document_GetField(Document *d, const char *fieldName) {
  if (!d || !fieldName) return NULL;

  for (int i = 0; i < d->numFields; i++) {
    if (!strcasecmp(d->fields[i].name, fieldName)) {
      return &d->fields[i];
    }
  }
  return NULL;
}
