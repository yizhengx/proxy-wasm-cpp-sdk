
kubectl delete ns hydragen
# istioctl install -f istio-operator.yaml -y
kubectl create ns hydragen
kubectl create configmap rate-limit-filter --from-file=rate-limit-filter.wasm="istio-counting-per-thread.wasm" -n hydragen
kubectl label namespace hydragen istio-injection=enabled
kubectl apply -f client.yaml

kubectl apply -f default-yamls

