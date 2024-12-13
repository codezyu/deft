#! /usr/bin/bash
# optional node 10.26.57.2 10.26.57.56 10.26.57.55
remote_node=("10.26.57.54" "10.26.57.55")
private_key="/home/zyu/.ssh/id_rsa_project"
cmd="cd /home/zyu/branch_code/deft; \
sh ./run.sh; sh "
#open local port as memcached port
sudo iptables -A INPUT -p tcp --dport 12348 -j ACCEPT
sudo iptables -A INPUT -p udp --dport 12348 -j ACCEPT
sudo iptables-save | sudo tee /etc/iptables/rules.v4
# make file
for node in ${remote_node[@]}
do
    ssh -i $private_key zyu@$node $cmd
done
# local start memcached
