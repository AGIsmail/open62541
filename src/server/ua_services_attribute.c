#include "ua_server_internal.h"
#include "ua_types_generated.h"
#include "ua_services.h"
#include "ua_statuscodes.h"
#include "ua_nodestore.h"
#include "ua_util.h"

/******************/
/* Read Attribute */
/******************/

#ifndef BUILD_UNIT_TESTS
static
#endif
UA_StatusCode parse_numericrange(const UA_String str, UA_NumericRange *range) {
    if(str.length < 0 || str.length >= 1023)
        return UA_STATUSCODE_BADINTERNALERROR;
#ifdef _MSC_VER
    char *cstring = (char*)UA_alloca(sizeof(char)*str.length+1);
#else
    char cstring[str.length+1];
#endif
    UA_memcpy(cstring, str.data, str.length);
    cstring[str.length] = 0;
    UA_Int32 index = 0;
    size_t dimensionsIndex = 0;
    size_t dimensionsMax = 3; // more should be uncommon, realloc if necessary
    struct UA_NumericRangeDimension *dimensions = UA_malloc(sizeof(struct UA_NumericRangeDimension) * 3);
    if(!dimensions)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    do {
        UA_Int32 min, max;
        UA_Int32 progress;
        UA_Int32 res = sscanf(&cstring[index], "%" SCNu32 "%n", &min, &progress);
        if(res <= 0 || min < 0) {
            retval = UA_STATUSCODE_BADINDEXRANGEINVALID;
            break;
        }
        index += progress;
        if(index >= str.length || cstring[index] == ',')
            max = min;
        else {
            res = sscanf(&cstring[index], ":%" SCNu32 "%n", &max, &progress);
            if(res <= 0 || max < 0 || min >= max) {
                retval = UA_STATUSCODE_BADINDEXRANGEINVALID;
                break;
            }
            index += progress;
        }
        
        if(dimensionsIndex >= dimensionsMax) {
            struct UA_NumericRangeDimension *newDimensions =
                UA_realloc(dimensions, sizeof(struct UA_NumericRangeDimension) * 2 * dimensionsMax);
            if(!newDimensions) {
                UA_free(dimensions);
                return UA_STATUSCODE_BADOUTOFMEMORY;
            }
            dimensions = newDimensions;
            dimensionsMax *= 2;
        }

        dimensions[dimensionsIndex].min = min;
        dimensions[dimensionsIndex].max = max;
        dimensionsIndex++;
    } while(retval == UA_STATUSCODE_GOOD && index + 1 < str.length && cstring[index] == ',' && ++index);

    if(retval != UA_STATUSCODE_GOOD) {
        UA_free(dimensions);
        return retval;
    }
        
    range->dimensions = dimensions;
    range->dimensionsSize = dimensionsIndex;
    return retval;
}

#define CHECK_NODECLASS(CLASS)                                  \
    if(!(node->nodeClass & (CLASS))) {                          \
        retval = UA_STATUSCODE_BADATTRIBUTEIDINVALID;           \
        break;                                                  \
    }

static void handleServerTimestamps(UA_TimestampsToReturn timestamps, UA_DataValue* v) {
	if(v && (timestamps == UA_TIMESTAMPSTORETURN_SERVER || timestamps == UA_TIMESTAMPSTORETURN_BOTH)) {
		v->hasServerTimestamp = UA_TRUE;
		v->serverTimestamp = UA_DateTime_now();
	}
}

static void handleSourceTimestamps(UA_TimestampsToReturn timestamps, UA_DataValue* v) {
	if(timestamps == UA_TIMESTAMPSTORETURN_SOURCE || timestamps == UA_TIMESTAMPSTORETURN_BOTH) {
		v->hasSourceTimestamp = UA_TRUE;
		v->sourceTimestamp = UA_DateTime_now();
	}
}

static UA_StatusCode getVariableNodeValue(const UA_VariableNode *vn, const UA_TimestampsToReturn timestamps,
                                          const UA_ReadValueId *id, UA_DataValue *v) {
    UA_NumericRange range;
    UA_NumericRange *rangeptr = UA_NULL;
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(id->indexRange.length > 0) {
        retval = parse_numericrange(id->indexRange, &range);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
        rangeptr = &range;
    }

    if(vn->valueSource == UA_VALUESOURCE_VARIANT) {
        if(vn->value.variant.callback.onRead)
            vn->value.variant.callback.onRead(vn->value.variant.callback.handle, vn->nodeId,
                                              &v->value, rangeptr);
        if(!rangeptr) {
            v->value = vn->value.variant.value;
            v->value.storageType = UA_VARIANT_DATA_NODELETE;
        } else {
            retval = UA_Variant_copyRange(&vn->value.variant.value, &v->value, range);
        }
        if(retval == UA_STATUSCODE_GOOD)
            handleSourceTimestamps(timestamps, v);
    } else {
        UA_Boolean sourceTimeStamp = (timestamps == UA_TIMESTAMPSTORETURN_SOURCE ||
                                      timestamps == UA_TIMESTAMPSTORETURN_BOTH);
        retval = vn->value.dataSource.read(vn->value.dataSource.handle, vn->nodeId,
                                           sourceTimeStamp, rangeptr, v);
    }

    if(rangeptr)
        UA_free(range.dimensions);
    return retval;
}

static UA_StatusCode getVariableNodeDataType(const UA_VariableNode *vn, UA_DataValue *v) {
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(vn->valueSource == UA_VALUESOURCE_VARIANT) {
        retval = UA_Variant_setScalarCopy(&v->value, &vn->value.variant.value.type->typeId,
                                          &UA_TYPES[UA_TYPES_NODEID]);
    } else {
        /* Read from the datasource to see the data type */
        UA_DataValue val;
        UA_DataValue_init(&val);
        retval = vn->value.dataSource.read(vn->value.dataSource.handle, vn->nodeId, UA_FALSE, UA_NULL, &val);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
        retval = UA_Variant_setScalarCopy(&v->value, &val.value.type->typeId, &UA_TYPES[UA_TYPES_NODEID]);
        UA_DataValue_deleteMembers(&val);
    }
    return retval;
}

static UA_StatusCode getVariableNodeArrayDimensions(const UA_VariableNode *vn, UA_DataValue *v) {
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(vn->valueSource == UA_VALUESOURCE_VARIANT) {
        retval = UA_Variant_setArrayCopy(&v->value, vn->value.variant.value.arrayDimensions,
                                         vn->value.variant.value.arrayDimensionsSize, &UA_TYPES[UA_TYPES_INT32]);
    } else {
        /* Read the datasource to see the array dimensions */
        UA_DataValue val;
        UA_DataValue_init(&val);
        retval = vn->value.dataSource.read(vn->value.dataSource.handle, vn->nodeId, UA_FALSE, UA_NULL, &val);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
        retval = UA_Variant_setArrayCopy(&v->value, val.value.arrayDimensions,
                                         val.value.arrayDimensionsSize, &UA_TYPES[UA_TYPES_INT32]);
        UA_DataValue_deleteMembers(&val);
    }
    return retval;
}

static const UA_String binEncoding = {sizeof("DefaultBinary")-1, (UA_Byte*)"DefaultBinary"};
/* clang complains about unused variables */
// static const UA_String xmlEncoding = {sizeof("DefaultXml")-1, (UA_Byte*)"DefaultXml"};

/** Reads a single attribute from a node in the nodestore. */
void Service_Read_single(UA_Server *server, UA_Session *session, const UA_TimestampsToReturn timestamps,
                         const UA_ReadValueId *id, UA_DataValue *v) {
	if(id->dataEncoding.name.length >= 0 && !UA_String_equal(&binEncoding, &id->dataEncoding.name)) {
           v->hasStatus = UA_TRUE;
           v->status = UA_STATUSCODE_BADDATAENCODINGINVALID;
           return;
	}

	//index range for a non-value
	if(id->indexRange.length >= 0 && id->attributeId != UA_ATTRIBUTEID_VALUE){
		v->hasStatus = UA_TRUE;
		v->status = UA_STATUSCODE_BADINDEXRANGENODATA;
		return;
	}

    UA_Node const *node = UA_NodeStore_get(server->nodestore, &id->nodeId);
    if(!node) {
        v->hasStatus = UA_TRUE;
        v->status = UA_STATUSCODE_BADNODEIDUNKNOWN;
        return;
    }

    /* When setting the value fails in the switch, we get an error code and set hasValue to false */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    v->hasValue = UA_TRUE;

    switch(id->attributeId) {
    case UA_ATTRIBUTEID_NODEID:
        retval = UA_Variant_setScalarCopy(&v->value, &node->nodeId, &UA_TYPES[UA_TYPES_NODEID]);
        break;
    case UA_ATTRIBUTEID_NODECLASS:
        retval = UA_Variant_setScalarCopy(&v->value, &node->nodeClass, &UA_TYPES[UA_TYPES_INT32]);
        break;
    case UA_ATTRIBUTEID_BROWSENAME:
        retval = UA_Variant_setScalarCopy(&v->value, &node->browseName, &UA_TYPES[UA_TYPES_QUALIFIEDNAME]);
        break;
    case UA_ATTRIBUTEID_DISPLAYNAME:
        retval = UA_Variant_setScalarCopy(&v->value, &node->displayName, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
        break;
    case UA_ATTRIBUTEID_DESCRIPTION:
        retval = UA_Variant_setScalarCopy(&v->value, &node->description, &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
        break;
    case UA_ATTRIBUTEID_WRITEMASK:
        retval = UA_Variant_setScalarCopy(&v->value, &node->writeMask, &UA_TYPES[UA_TYPES_UINT32]);
        break;
    case UA_ATTRIBUTEID_USERWRITEMASK:
        retval = UA_Variant_setScalarCopy(&v->value, &node->userWriteMask, &UA_TYPES[UA_TYPES_UINT32]);
        break;
    case UA_ATTRIBUTEID_ISABSTRACT:
        CHECK_NODECLASS(UA_NODECLASS_REFERENCETYPE | UA_NODECLASS_OBJECTTYPE |
                        UA_NODECLASS_VARIABLETYPE | UA_NODECLASS_DATATYPE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_ReferenceTypeNode *)node)->isAbstract,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case UA_ATTRIBUTEID_SYMMETRIC:
        CHECK_NODECLASS(UA_NODECLASS_REFERENCETYPE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_ReferenceTypeNode *)node)->symmetric,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case UA_ATTRIBUTEID_INVERSENAME:
        CHECK_NODECLASS(UA_NODECLASS_REFERENCETYPE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_ReferenceTypeNode *)node)->inverseName,
                                          &UA_TYPES[UA_TYPES_LOCALIZEDTEXT]);
        break;
    case UA_ATTRIBUTEID_CONTAINSNOLOOPS:
        CHECK_NODECLASS(UA_NODECLASS_VIEW);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_ViewNode *)node)->containsNoLoops,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case UA_ATTRIBUTEID_EVENTNOTIFIER:
        CHECK_NODECLASS(UA_NODECLASS_VIEW | UA_NODECLASS_OBJECT);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_ViewNode *)node)->eventNotifier,
                                          &UA_TYPES[UA_TYPES_BYTE]);
        break;
    case UA_ATTRIBUTEID_VALUE:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLETYPE);
        retval = getVariableNodeValue((const UA_VariableNode*)node, timestamps, id, v);
        break;
    case UA_ATTRIBUTEID_DATATYPE:
		CHECK_NODECLASS(UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLETYPE);
        retval = getVariableNodeDataType((const UA_VariableNode*)node, v);
        break;
    case UA_ATTRIBUTEID_VALUERANK:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLETYPE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_VariableTypeNode *)node)->valueRank,
                                           &UA_TYPES[UA_TYPES_INT32]);
        break;
    case UA_ATTRIBUTEID_ARRAYDIMENSIONS:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLETYPE);
        retval = getVariableNodeArrayDimensions((const UA_VariableNode*)node, v);
        break;
    case UA_ATTRIBUTEID_ACCESSLEVEL:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_VariableNode*)node)->accessLevel,
                                          &UA_TYPES[UA_TYPES_BYTE]);
        break;
    case UA_ATTRIBUTEID_USERACCESSLEVEL:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_VariableNode*)node)->userAccessLevel,
                                          &UA_TYPES[UA_TYPES_BYTE]);
        break;
    case UA_ATTRIBUTEID_MINIMUMSAMPLINGINTERVAL:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_VariableNode*)node)->minimumSamplingInterval,
                                          &UA_TYPES[UA_TYPES_DOUBLE]);
        break;
    case UA_ATTRIBUTEID_HISTORIZING:
        CHECK_NODECLASS(UA_NODECLASS_VARIABLE);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_VariableNode *)node)->historizing,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case UA_ATTRIBUTEID_EXECUTABLE:
        CHECK_NODECLASS(UA_NODECLASS_METHOD);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_MethodNode *)node)->executable,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    case UA_ATTRIBUTEID_USEREXECUTABLE:
        CHECK_NODECLASS(UA_NODECLASS_METHOD);
        retval = UA_Variant_setScalarCopy(&v->value, &((const UA_MethodNode *)node)->userExecutable,
                                          &UA_TYPES[UA_TYPES_BOOLEAN]);
        break;
    default:
        retval = UA_STATUSCODE_BADATTRIBUTEIDINVALID;
        break;
    }

    if(retval != UA_STATUSCODE_GOOD) {
        v->hasValue = UA_FALSE;
        v->hasStatus = UA_TRUE;
        v->status = retval;
    }

    // Todo: what if the timestamp from the datasource are already present?
    handleServerTimestamps(timestamps, v);
}

void Service_Read(UA_Server *server, UA_Session *session, const UA_ReadRequest *request,
                  UA_ReadResponse *response) {
    if(request->nodesToReadSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    if(request->timestampsToReturn > 3){
    	response->responseHeader.serviceResult = UA_STATUSCODE_BADTIMESTAMPSTORETURNINVALID;
    	return;
    }

    size_t size = request->nodesToReadSize;

    response->results = UA_Array_new(&UA_TYPES[UA_TYPES_DATAVALUE], size);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

    response->resultsSize = size;

    if(request->maxAge < 0) {
    	response->responseHeader.serviceResult = UA_STATUSCODE_BADMAXAGEINVALID;
        return;
    }

#ifdef UA_EXTERNAL_NAMESPACES
    UA_Boolean isExternal[size];
    UA_UInt32 indices[size];
    UA_memset(isExternal, UA_FALSE, sizeof(UA_Boolean) * size);
    for(size_t j = 0;j<server->externalNamespacesSize;j++) {
        size_t indexSize = 0;
        for(size_t i = 0;i < size;i++) {
            if(request->nodesToRead[i].nodeId.namespaceIndex != server->externalNamespaces[j].index)
                continue;
            isExternal[i] = UA_TRUE;
            indices[indexSize] = i;
            indexSize++;
        }
        if(indexSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->readNodes(ens->ensHandle, &request->requestHeader, request->nodesToRead,
                       indices, indexSize, response->results, UA_FALSE, response->diagnosticInfos);
    }
#endif

    for(size_t i = 0;i < size;i++) {
#ifdef UA_EXTERNAL_NAMESPACES
        if(!isExternal[i])
#endif
            Service_Read_single(server, session, request->timestampsToReturn,
                                &request->nodesToRead[i], &response->results[i]);
    }

#ifdef EXTENSION_STATELESS
    if(session==&anonymousSession){
		/* expiry header */
		UA_ExtensionObject additionalHeader;
		UA_ExtensionObject_init(&additionalHeader);
		additionalHeader.typeId = UA_TYPES[UA_TYPES_VARIANT].typeId;
		additionalHeader.encoding = UA_EXTENSIONOBJECT_ENCODINGMASK_BODYISBYTESTRING;

		UA_Variant variant;
		UA_Variant_init(&variant);

		UA_DateTime* expireArray = UA_NULL;
		expireArray = UA_Array_new(&UA_TYPES[UA_TYPES_DATETIME], request->nodesToReadSize);
		variant.data = expireArray;

		/*expires in 20 seconds*/
		for(UA_Int32 i = 0;i < response->resultsSize;i++) {
			expireArray[i] = UA_DateTime_now() + 20 * 100 * 1000 * 1000;
		}
		UA_Variant_setArray(&variant, expireArray, request->nodesToReadSize, &UA_TYPES[UA_TYPES_DATETIME]);

		size_t offset = 0;
		UA_ByteString str;
        UA_ByteString_newMembers(&str, 65536);
		UA_Variant_encodeBinary(&variant, &str, &offset);

        UA_Array_delete(expireArray, &UA_TYPES[UA_TYPES_DATETIME], request->nodesToReadSize);

		additionalHeader.body = str;
		additionalHeader.body.length = offset;
		response->responseHeader.additionalHeader = additionalHeader;
    }
#endif
}

/*******************/
/* Write Attribute */
/*******************/

UA_StatusCode UA_Server_editNode(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
                                 UA_EditNodeCallback callback, const void *data) {
    UA_StatusCode retval;
    do {
        retval = UA_STATUSCODE_GOOD;
        const UA_Node *node = UA_NodeStore_get(server->nodestore, nodeId);
        if(!node)
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
#ifndef UA_MULTITHREADING
        retval = callback(server, session, (UA_Node*)(uintptr_t)node, data);
        return retval;
#else
        UA_Node *copy = UA_Node_copyAnyNodeClass(node);
        if(!copy)
            return UA_STATUSCODE_BADOUTOFMEMORY;
        retval = callback(server, session, copy, data);
        if(retval != UA_STATUSCODE_GOOD) {
            UA_Node_deleteAnyNodeClass(copy);
            return retval;
        }
        retval = UA_NodeStore_replace(server->nodestore, orig, copy, UA_NULL);
        if(retval != UA_STATUSCODE_GOOD)
            UA_Node_deleteAnyNodeClass(copy);
#endif
    } while(retval != UA_STATUSCODE_GOOD);
    return UA_STATUSCODE_GOOD;
}

#define CHECK_DATATYPE(EXP_DT)                                          \
    if(!wvalue->value.hasValue ||                                       \
       &UA_TYPES[UA_TYPES_##EXP_DT] != wvalue->value.value.type ||      \
       !UA_Variant_isScalar(&wvalue->value.value)) {                    \
        retval = UA_STATUSCODE_BADTYPEMISMATCH;                         \
        break;                                                          \
    }

#define CHECK_NODECLASS_WRITE(CLASS)                                    \
    if((node->nodeClass & (CLASS)) == 0) {                              \
        retval = UA_STATUSCODE_BADNODECLASSINVALID;                     \
        break;                                                          \
    }

static UA_StatusCode
Service_Write_single_ValueDataSource(UA_Server *server, UA_Session *session, const UA_VariableNode *node,
                                     UA_WriteValue *wvalue) {
    UA_assert(wvalue->attributeId == UA_ATTRIBUTEID_VALUE);
    UA_assert(node->nodeClass == UA_NODECLASS_VARIABLE || node->nodeClass == UA_NODECLASS_VARIABLETYPE);
    UA_assert(node->valueSource == UA_VALUESOURCE_DATASOURCE);

    UA_StatusCode retval;
    if(wvalue->indexRange.length <= 0) {
        retval = node->value.dataSource.write(node->value.dataSource.handle, node->nodeId,
                                              &wvalue->value.value, UA_NULL);
    } else {
        UA_NumericRange range;
        retval = parse_numericrange(wvalue->indexRange, &range);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
        retval = node->value.dataSource.write(node->value.dataSource.handle, node->nodeId,
                                              &wvalue->value.value, &range);
        UA_free(range.dimensions);
    }
    return retval;
}

/* In the multithreaded case, node is a copy */
static UA_StatusCode
MoveValueIntoNode(UA_Server *server, UA_Session *session, UA_VariableNode *node, UA_WriteValue *wvalue) {
    UA_assert(wvalue->attributeId == UA_ATTRIBUTEID_VALUE);
    UA_assert(node->nodeClass == UA_NODECLASS_VARIABLE || node->nodeClass == UA_NODECLASS_VARIABLETYPE);
    UA_assert(node->valueSource == UA_VALUESOURCE_VARIANT);

    /* Parse the range */
    UA_NumericRange range;
    UA_NumericRange *rangeptr = UA_NULL;
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(wvalue->indexRange.length > 0) {
        retval = parse_numericrange(wvalue->indexRange, &range);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
        rangeptr = &range;
    }

    /* The nodeid on the wire may be != the nodeid in the node: opaque types, enums and bytestrings.
       nodeV contains the correct type definition. */
    UA_Variant *newV = &wvalue->value.value;
    UA_Variant *oldV = &node->value.variant.value;
    UA_Variant cast_v;
    if(!UA_NodeId_equal(&oldV->type->typeId, &newV->type->typeId)) {
        cast_v = wvalue->value.value;
        newV = &cast_v;
        if(oldV->type->namespaceZero && newV->type->namespaceZero &&
           oldV->type->typeIndex == newV->type->typeIndex) {
            /* An enum was sent as an int32, or an opaque type as a bytestring. This is
               detected with the typeIndex indicated the "true" datatype. */
            newV->type = oldV->type;
        } else if(oldV->type == &UA_TYPES[UA_TYPES_BYTE] && !UA_Variant_isScalar(oldV) &&
                  newV->type == &UA_TYPES[UA_TYPES_BYTESTRING] && UA_Variant_isScalar(newV)) {
            /* a string is written to a byte array */
            UA_ByteString *str = (UA_ByteString*) newV->data;
            newV->arrayLength = str->length;
            newV->data = str->data;
            newV->type = &UA_TYPES[UA_TYPES_BYTE];
        } else {
            if(rangeptr)
                UA_free(range.dimensions);
            return UA_STATUSCODE_BADTYPEMISMATCH;
        }
    }
    
    if(!rangeptr) {
        // TODO: Avoid copying the whole node and then delete the old value for multithreading
        UA_Variant_deleteMembers(&node->value.variant.value);
        node->value.variant.value = *newV;
        UA_Variant_init(&wvalue->value.value);
    } else {
        retval = UA_Variant_setRangeCopy(&node->value.variant.value, newV->data, newV->arrayLength, range);
    }
    if(node->value.variant.callback.onWrite)
        node->value.variant.callback.onWrite(node->value.variant.callback.handle, node->nodeId,
                                             &node->value.variant.value, rangeptr);

    if(rangeptr)
        UA_free(range.dimensions);
    return retval;
}

static UA_StatusCode
MoveAttributeIntoNode(UA_Server *server, UA_Session *session, UA_Node *node, UA_WriteValue *wvalue) {
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    void *value = wvalue->value.value.data;
	switch(wvalue->attributeId) {
    case UA_ATTRIBUTEID_NODEID:
    case UA_ATTRIBUTEID_NODECLASS:
    case UA_ATTRIBUTEID_DATATYPE:
		retval = UA_STATUSCODE_BADWRITENOTSUPPORTED;
		break;
	case UA_ATTRIBUTEID_BROWSENAME:
		CHECK_DATATYPE(QUALIFIEDNAME);
		UA_QualifiedName_deleteMembers(&node->browseName);
        node->browseName = *(UA_QualifiedName*)value;
        UA_QualifiedName_init((UA_QualifiedName*)value);
		break;
	case UA_ATTRIBUTEID_DISPLAYNAME:
		CHECK_DATATYPE(LOCALIZEDTEXT);
		UA_LocalizedText_deleteMembers(&node->displayName);
        node->displayName = *(UA_LocalizedText*)value;
		UA_LocalizedText_init((UA_LocalizedText*)value);
		break;
	case UA_ATTRIBUTEID_DESCRIPTION:
		CHECK_DATATYPE(LOCALIZEDTEXT);
		UA_LocalizedText_deleteMembers(&node->description);
        node->description = *(UA_LocalizedText*)value;
		UA_LocalizedText_init((UA_LocalizedText*)value);
		break;
	case UA_ATTRIBUTEID_WRITEMASK:
		CHECK_DATATYPE(UINT32);
		node->writeMask = *(UA_UInt32*)value;
		break;
	case UA_ATTRIBUTEID_USERWRITEMASK:
		CHECK_DATATYPE(UINT32);
		node->userWriteMask = *(UA_UInt32*)value;
		break;    
	case UA_ATTRIBUTEID_ISABSTRACT:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_OBJECTTYPE | UA_NODECLASS_REFERENCETYPE |
                              UA_NODECLASS_VARIABLETYPE | UA_NODECLASS_DATATYPE);
		CHECK_DATATYPE(BOOLEAN);
		((UA_ObjectTypeNode*)node)->isAbstract = *(UA_Boolean*)value;
		break;
	case UA_ATTRIBUTEID_SYMMETRIC:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_REFERENCETYPE);
		CHECK_DATATYPE(BOOLEAN);
		((UA_ReferenceTypeNode*)node)->symmetric = *(UA_Boolean*)value;
		break;
	case UA_ATTRIBUTEID_INVERSENAME:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_REFERENCETYPE);
		CHECK_DATATYPE(LOCALIZEDTEXT);
        UA_ReferenceTypeNode *n = (UA_ReferenceTypeNode*)node;
		UA_LocalizedText_deleteMembers(&n->inverseName);
        n->inverseName = *(UA_LocalizedText*)value;
		UA_LocalizedText_init((UA_LocalizedText*)value);
		break;
	case UA_ATTRIBUTEID_CONTAINSNOLOOPS:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VIEW);
		CHECK_DATATYPE(BOOLEAN);
        ((UA_ViewNode*)node)->containsNoLoops = *(UA_Boolean*)value;
		break;
	case UA_ATTRIBUTEID_EVENTNOTIFIER:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VIEW | UA_NODECLASS_OBJECT);
		CHECK_DATATYPE(BYTE);
        ((UA_ViewNode*)node)->eventNotifier = *(UA_Byte*)value;
		break;
	case UA_ATTRIBUTEID_VALUE:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLETYPE);
        retval = MoveValueIntoNode(server, session, (UA_VariableNode*)node, wvalue);
		break;
	case UA_ATTRIBUTEID_ACCESSLEVEL:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VARIABLE);
		CHECK_DATATYPE(BYTE);
		((UA_VariableNode*)node)->accessLevel = *(UA_Byte*)value;
		break;
	case UA_ATTRIBUTEID_USERACCESSLEVEL:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VARIABLE);
		CHECK_DATATYPE(BYTE);
		((UA_VariableNode*)node)->userAccessLevel = *(UA_Byte*)value;
		break;
	case UA_ATTRIBUTEID_MINIMUMSAMPLINGINTERVAL:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VARIABLE);
		CHECK_DATATYPE(DOUBLE);
		((UA_VariableNode*)node)->minimumSamplingInterval = *(UA_Double*)value;
		break;
	case UA_ATTRIBUTEID_HISTORIZING:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_VARIABLE);
		CHECK_DATATYPE(BOOLEAN);
		((UA_VariableNode*)node)->historizing = *(UA_Boolean*)value;
		break;
	case UA_ATTRIBUTEID_EXECUTABLE:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_METHOD);
		CHECK_DATATYPE(BOOLEAN);
		((UA_MethodNode*)node)->executable = *(UA_Boolean*)value;
		break;
	case UA_ATTRIBUTEID_USEREXECUTABLE:
		CHECK_NODECLASS_WRITE(UA_NODECLASS_METHOD);
		CHECK_DATATYPE(BOOLEAN);
		((UA_MethodNode*)node)->userExecutable = *(UA_Boolean*)value;
		break;
	default:
		retval = UA_STATUSCODE_BADATTRIBUTEIDINVALID;
		break;
	}
    return retval;
}

UA_StatusCode Service_Write_single(UA_Server *server, UA_Session *session, UA_WriteValue *wvalue) {
    if(!wvalue->value.hasValue || !wvalue->value.value.data)
        return UA_STATUSCODE_BADNODATA; // TODO: is this the right return code?
    if(wvalue->attributeId == UA_ATTRIBUTEID_VALUE) {
        const UA_Node *orig = UA_NodeStore_get(server->nodestore, &wvalue->nodeId);
        if(!orig)
            return UA_STATUSCODE_BADNODEIDUNKNOWN;
        if(orig->nodeClass & (UA_NODECLASS_VARIABLE | UA_NODECLASS_VARIABLE) &&
           ((const UA_VariableNode*)orig)->valueSource == UA_VALUESOURCE_DATASOURCE) {
            UA_StatusCode retval = Service_Write_single_ValueDataSource(server, session,
                                                                        (const UA_VariableNode*)orig, wvalue);
            return retval;
        }
    }
    return UA_Server_editNode(server, session, &wvalue->nodeId,
                              (UA_EditNodeCallback)MoveAttributeIntoNode, wvalue);
}

void Service_Write(UA_Server *server, UA_Session *session, const UA_WriteRequest *request,
                   UA_WriteResponse *response) {
    UA_assert(server != UA_NULL && session != UA_NULL && request != UA_NULL && response != UA_NULL);

    if(request->nodesToWriteSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_Array_new(&UA_TYPES[UA_TYPES_STATUSCODE], request->nodesToWriteSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

#ifdef UA_EXTERNAL_NAMESPACES
    UA_Boolean isExternal[request->nodesToWriteSize];
    UA_UInt32 indices[request->nodesToWriteSize];
    UA_memset(isExternal, UA_FALSE, sizeof(UA_Boolean)*request->nodesToWriteSize);
    for(size_t j = 0; j < server->externalNamespacesSize; j++) {
        UA_UInt32 indexSize = 0;
        for(UA_Int32 i = 0; i < request->nodesToWriteSize; i++) {
            if(request->nodesToWrite[i].nodeId.namespaceIndex !=
               server->externalNamespaces[j].index)
                continue;
            isExternal[i] = UA_TRUE;
            indices[indexSize] = i;
            indexSize++;
        }
        if(indexSize == 0)
            continue;
        UA_ExternalNodeStore *ens = &server->externalNamespaces[j].externalNodeStore;
        ens->writeNodes(ens->ensHandle, &request->requestHeader, request->nodesToWrite,
                        indices, indexSize, response->results, response->diagnosticInfos);
    }
#endif
    
    response->resultsSize = request->nodesToWriteSize;
    for(UA_Int32 i = 0;i < request->nodesToWriteSize;i++) {
#ifdef UA_EXTERNAL_NAMESPACES
        if(!isExternal[i])
#endif
		  response->results[i] = Service_Write_single(server, session, &request->nodesToWrite[i]);
    }
}
