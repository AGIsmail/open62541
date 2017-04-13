/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information. */

#ifdef UA_NO_AMALGAMATION
# include "ua_types.h"
# include "ua_client.h"
# include "ua_client_highlevel.h"
# include "ua_nodeids.h"
# include "ua_network_tcp.h"
# include "ua_config_standard.h"
#else
# include "open62541.h"
# include <string.h>
# include <stdlib.h>
#endif

#include <stdio.h>
#include <assert.h>

int main(int argc, char *argv[]) {
    UA_Client *client = UA_Client_new(UA_ClientConfig_standard);

    /* Listing endpoints */
    UA_EndpointDescription* endpointArray = NULL;
    size_t endpointArraySize = 0;
    UA_StatusCode retval = UA_Client_getEndpoints(client, "opc.tcp://opcua.demo-this.com:51210/UA/SampleServer",
                                                  &endpointArraySize, &endpointArray);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_Array_delete(endpointArray, endpointArraySize, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);
        UA_Client_delete(client);
        return (int)retval;
    }
    printf("%i endpoints found\n", (int)endpointArraySize);
    for(size_t i=0;i<endpointArraySize;i++){
        printf("URL of endpoint %i is %.*s\n", (int)i,
               (int)endpointArray[i].endpointUrl.length,
               endpointArray[i].endpointUrl.data);
    }
    UA_Array_delete(endpointArray,endpointArraySize, &UA_TYPES[UA_TYPES_ENDPOINTDESCRIPTION]);

    /* Connect to a server */
    /* anonymous connect would be: retval = UA_Client_connect(client, "opc.tcp://localhost:16664"); */
    retval = UA_Client_connect(client, "opc.tcp://opcua.demo-this.com:51210/UA/SampleServer");
    /* for when encryption is supported I guess - Issue #788 */
//    retval = UA_Client_connect_username(client, "opc.tcp://opcua.demo-this.com:51210/UA/SampleServer", "User", "Password");
    if(retval != UA_STATUSCODE_GOOD) {
        UA_Client_delete(client);
        return (int)retval;
    }

    /* Read attribute */
    printf("\nReading the value of node (0, UA_NS0ID_SERVER_NAMESPACEARRAY):\n");
    UA_Variant *valString = UA_Variant_new();
    retval = UA_Client_readValueAttribute(client, UA_NODEID_NUMERIC(0, UA_NS0ID_SERVER_NAMESPACEARRAY), valString);
    if(retval == UA_STATUSCODE_GOOD && !UA_Variant_isScalar(valString) &&
       valString->type == &UA_TYPES[UA_TYPES_STRING]) {
            if (valString->arrayDimensionsSize == 0){ /* The array should be 1D */
                /* Initialize buffers */
                UA_UInt16 nsI = 13; /* Holds the namespace Index */
                char *nsUriData = malloc(65535 * sizeof(UA_Byte));
                UA_String nsUri; /* Holds the namespace Uri and Uri string length*/
                for(size_t j=0; j < (valString->arrayLength); j++){ /* Everything is stored in an array in the end */
                    nsUri.length = (size_t) snprintf(nsUriData, 65535, "%.*s",
                                                (int) ((UA_String *)valString->data)[j].length,
                                                ((UA_String *)valString->data)[j].data);
                    nsUri.data = (UA_Byte *) nsUriData;
                    printf("Testing UA_Client_NamespaceGetIndex() using the URI %.*s \n", 
                        (int) nsUri.length, nsUri.data);
                    retval = UA_Client_NamespaceGetIndex(client, &nsUri, &nsI);
                    if(retval == UA_STATUSCODE_GOOD)
                        printf("Testing UA_Client_NamespaceGetIndex() - nsI = %d\n", nsI);
                    else
                        printf("Testing UA_Client_NamespaceGetIndex(): Bad status code returned %08x\n", retval);
                    memset(nsUriData, 0, 65535);
                }
                free (nsUriData);
            }
    }

    UA_Variant_delete(valString);

    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return (int) UA_STATUSCODE_GOOD;
}
