import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME

CODEOWNERS = ["@jethome-iot"]
GROUPS_STORAGE_ID = "groups_storage_id"
CONF_ENTITIES = "entities"
CONF_GROUPS = "groups"

groups_ns = cg.esphome_ns.namespace("groups")
GroupClass = groups_ns.class_("Group")
GroupStorage = groups_ns.class_("GroupsStorage")

GROUP_BASE_SCHEMA = {
    cv.Required(CONF_ID): cv.declare_id(GroupClass),
    cv.Optional(CONF_NAME): cv.string,
    cv.Optional(CONF_ENTITIES): cv.ensure_list(cv.use_id(cg.EntityBase)),
}

GROUP_ID_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.use_id(cg.int_),
    }
)

# Schema for components that dynamically create entities and need to assign them to groups
# (e.g., dallas_temp_searcher which discovers sensors at runtime)
LIST_OF_GROUPS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_GROUPS): cv.All(
            cv.ensure_list(cv.use_id(GroupClass)), cv.Length(min=1)
        )
    }
)

CONFIG_SCHEMA = cv.All(cv.ensure_list(GROUP_BASE_SCHEMA))


async def add_groups_to_storage(storage_var, config):
    for group_config in config:
        group_var = await cg.get_variable(group_config)
        cg.add(storage_var.add_group(group_var))


async def to_code(config):
    var = cg.new_Pvariable(cv.declare_id(GroupStorage)(GROUPS_STORAGE_ID))

    for group_config in config:
        group_var = cg.new_Pvariable(group_config[CONF_ID])
        cg.add(group_var.set_group_name(group_config[CONF_NAME]))
        # Add entities to group
        for entity_id in group_config.get(CONF_ENTITIES, []):
            entity_var = await cg.get_variable(entity_id)
            cg.add(group_var.add_entity(entity_var))
        cg.add(var.add_group(group_var))
    cg.add_define("USE_GROUPS")
