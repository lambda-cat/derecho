

#include <iostream>
#include "derecho/derecho.h"

#ifdef __CDT_PARSER__
#define REGISTER_RPC_FUNCTIONS(...)
#define RPC_NAME(...) 0ULL
#endif


using namespace persistent;
using derecho::Replicated;

/** A test object like PersistentThing in total_restart_test,
 * but whose state can be much larger than an int. */
class TestObject : public mutils::ByteRepresentable, public derecho::PersistsFields {
    Persistent<std::string> state;

public:
    TestObject(Persistent<std::string>& init_state) : state(std::move(init_state)) {}
    TestObject(PersistentRegistry* registry) : state([](){return std::make_unique<std::string>();},
                                                     nullptr,
                                                     registry) {}
    void update(const std::string& new_state) {
        *state = new_state;
    }
    DEFAULT_SERIALIZATION_SUPPORT(TestObject, state);
    REGISTER_RPC_FUNCTIONS(TestObject, update);

};

std::string make_random_string(uint length) {
    std::stringstream string;
    for(uint i = 0; i < length; ++i) {
        char next = 'a'+ (rand() % 26);
        string << next;
    }
    return string.str();
}

int main(int argc, char** argv) {
    //Get custom arguments from the end of the arguments list
    const uint num_shards = std::stoi(argv[argc - 4]);
    const uint nodes_per_shard = std::stoi(argv[argc - 3]);
    const uint updates_behind = std::stoi(argv[argc - 2]);
    const uint update_size = std::stoi(argv[argc - 1]);
    unsigned int pid = getpid();
    srand(pid);

    derecho::Conf::initialize(argc, argv);

    //derecho::SubgroupInfo subgroup_config(derecho::DefaultSubgroupAllocator(
    //        {{std::type_index(typeid(TestObject)),
    //            derecho::one_subgroup_policy(derecho::even_sharding_policy(num_shards,nodes_per_shard))
    //        }}));
    //Hacky custom allocator to allow shards to continue operating with 1 failed node
    derecho::SubgroupInfo subgroup_config([num_shards, nodes_per_shard](const std::type_index& subgroup_type,
            const std::unique_ptr<derecho::View>& prev_view,
            derecho::View& curr_view) {
        const uint num_subgroups = 1;
        const uint minimum_members_per_shard = nodes_per_shard - 1;
        derecho::subgroup_shard_layout_t layout_vector(num_subgroups);
        std::set<node_id_t> sorted_view_members(curr_view.members.begin(), curr_view.members.end());
        for(uint subgroup_num = 0; subgroup_num < num_subgroups; ++subgroup_num) {
            for(uint shard_num = 0; shard_num < num_shards; ++shard_num) {
                //For testing only, the node IDs for each shard must be members_per_shard sequential integers,
                //starting at 0 for the first shard. i.e. {0, 1, 2}, {3, 4, 5} for 3-member shards
                std::set<node_id_t> desired_members;
                for(uint member_index = 0; member_index < nodes_per_shard; ++member_index) {
                    desired_members.emplace((subgroup_num * num_shards * nodes_per_shard)
                                            + (shard_num * nodes_per_shard)
                                            + member_index);
                }
                std::vector<node_id_t> subview_members;
                std::set_intersection(desired_members.begin(), desired_members.end(),
                                      sorted_view_members.begin(), sorted_view_members.end(),
                                      std::back_inserter(subview_members));
                if(subview_members.size() < minimum_members_per_shard) {
                    throw derecho::subgroup_provisioning_exception();
                }
                layout_vector[subgroup_num].push_back(curr_view.make_subview(subview_members));
            }
        }
        return layout_vector;
    });

    derecho::Group<TestObject> group(derecho::CallbackSet{nullptr}, subgroup_config, nullptr,
                                     std::vector<derecho::view_upcall_t>{},
                                     [](PersistentRegistry* pr){ return std::make_unique<TestObject>(pr); });

    std::cout << "Waiting for all " << (num_shards * nodes_per_shard) << " members to join before starting updates" << std::endl;
    while(group.get_members().size() < num_shards * nodes_per_shard) {

    }

    std::vector<node_id_t> my_shard_members = group.get_subgroup_members<TestObject>(0).at(
            group.get_my_shard<TestObject>(0));
    uint my_shard_rank;
    for(my_shard_rank = 0; my_shard_rank < my_shard_members.size(); ++my_shard_rank) {
        if(my_shard_members[my_shard_rank] == derecho::getConfUInt32(CONF_DERECHO_LOCAL_ID))
            break;
    }
    Replicated<TestObject>& obj_handle = group.get_subgroup<TestObject>();
    uint num_updates = 10000;
    for(uint counter = 0; counter < num_updates - updates_behind; ++counter) {
        std::string new_value = make_random_string(update_size);
        derecho::rpc::QueryResults<void> result = obj_handle.ordered_send<RPC_NAME(update)>(new_value);
        //Wait for the last message to be sent
        if(counter == num_updates - updates_behind) {
            result.get();
        }
    }
    std::cout << (num_updates - updates_behind) << " updates sent" << std::endl;
    //The second node in each shard will exit early
    if(my_shard_rank == 1) {
        std::cout << "All updates sent, exiting early" << std::endl;
        exit(0);
    }
    else {
        for(uint counter = 0; counter < updates_behind; ++counter) {
            std::string new_value = make_random_string(update_size);
            derecho::rpc::QueryResults<void> result = obj_handle.ordered_send<RPC_NAME(update)>(new_value);
            //Wait for the last message to be sent
            if(counter == updates_behind) {
                result.get();
            }
        }
    }
    std::cout << "All updates sent, spinning" << std::endl;
    while(true) {
    }
    return 0;
}
