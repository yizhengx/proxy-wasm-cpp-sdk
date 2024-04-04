#!/bin/bash
action=$1
app=$2
delay=$3
processing_time=$4
prob=$5
kubectl $action -n hydragen -f - <<EOF
apiVersion: networking.istio.io/v1alpha3
kind: EnvoyFilter
metadata:
  name: $app-rate-limit-filter
spec:
  workloadSelector:
    labels:
      app: $app
      version: ""
  configPatches:
  - applyTo: HTTP_FILTER
    match:
      context: SIDECAR_INBOUND
      listener:
        filterChain:
          filter:
            name: envoy.http_connection_manager
            subFilter:
              name: envoy.router
    patch:
      operation: INSERT_BEFORE
      value:
        name: mydummy
        typed_config:
          '@type': type.googleapis.com/udpa.type.v1.TypedStruct
          type_url: type.googleapis.com/envoy.extensions.filters.http.wasm.v3.Wasm
          value:
            config:
              configuration:
                '@type': type.googleapis.com/google.protobuf.StringValue
                value: '{"delay": "${delay}", "processing_time": "${processing_time}", "probability": "${prob}"}'
              root_id: "dcoz_root_id"
              vm_config:
                code:
                  local:
                    filename: /var/local/wasm/rate-limit-filter.wasm
                runtime: envoy.wasm.runtime.v8
                vm_id: dcoz-vm
EOF