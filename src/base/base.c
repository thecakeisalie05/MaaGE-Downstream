#include "base_internal.h"
#include "maage/gameplay.h"
#include "maage/session_protocol.h"
#include <stdlib.h>
#include <string.h>

const maage_base_ids *maage_base_generation_references(const maage_block_registry *registry);
struct maage_base_lod_sampler {
    const maage_base_ids *ids;
    const maage_biome_registry *biomes;
    maage_world_seed seed;
    maage_generation_policy policy;
    maage_generation_region_cache *regions;
};
maage_base_lod_sampler *maage_base_lod_sampler_create(
    const maage_block_registry *blocks, const maage_biome_registry *biomes,
    maage_world_seed seed, const maage_generation_policy *policy) {
    maage_base_lod_sampler *sampler;
    const maage_base_ids *ids;
    if (!blocks || !biomes) return NULL;
    ids = maage_base_generation_references(blocks);
    if (!ids) return NULL;
    sampler = (maage_base_lod_sampler *) calloc(1, sizeof(*sampler));
    if (!sampler) return NULL;
    sampler->regions = (maage_generation_region_cache *) calloc(1, sizeof(*sampler->regions));
    if (!sampler->regions) {
        free(sampler);
        return NULL;
    }
    sampler->ids = ids;
    sampler->biomes = biomes;
    sampler->seed = seed;
    if (policy) sampler->policy = *policy;
    else maage_generation_policy_default(&sampler->policy);
    if (!maage_generation_policy_valid(&sampler->policy)) {
        maage_base_lod_sampler_destroy(sampler);
        return NULL;
    }
    return sampler;
}
int maage_base_lod_sampler_sample(maage_base_lod_sampler *sampler,
    int x, int z, maage_base_lod_column *column) {
    maage_generation_sample sample;
    if (!sampler || !column || x < -MAAGE_WORLD_LIMIT || x > MAAGE_WORLD_LIMIT ||
        z < -MAAGE_WORLD_LIMIT || z > MAAGE_WORLD_LIMIT) return 0;
    if (!maage_generation_sample_at(sampler->ids, sampler->seed, sampler->biomes,
            &sampler->policy, x, z, &sample, sampler->regions)) return 0;
    column->canopy_height = 0;
    column->canopy = sampler->ids->state_leaves;
    /* Macro canopy cover follows biome decoration density on stable 8-block
     * cells. This preserves forest masses, not individual generated trees. */
    if (sample.height > sampler->policy.sea_level) {
        int gx = x / 8, gz = z / 8, lx, lz;
        unsigned long hash;
        double density;
        if (x % 8 < 0) --gx;
        if (z % 8 < 0) --gz;
        lx = x - gx * 8;
        lz = z - gz * 8;
        hash = maage_generation_feature_hash(sampler->seed, gx, gz, 14000UL);
        density = sample.decoration.trees[0] + sample.decoration.trees[1] + sample.decoration.trees[2];
        if ((double) (hash & 65535UL) / 65536.0 < density && lx > 0 && lx < 7 && lz > 0 && lz < 7) column->canopy_height = sample.height + 5 + (int) ((hash >> 16) & 3UL);
    }
    column->height = sample.height + 1;
    column->surface = sample.surface;
    column->biome = sample.biome;
    column->water_height = sample.height < sampler->policy.sea_level ? sampler->policy.sea_level + 1 : 0;
    column->water = column->water_height ? sampler->ids->state_water : 0;
    return 1;
}
void maage_base_lod_sampler_destroy(maage_base_lod_sampler *sampler) {
    if (!sampler) return;
    free(sampler->regions);
    free(sampler);
}

const maage_base_ids *maage_base_generation_references(const maage_block_registry *registry) {
    const maage_registry_bindings *bindings = maage_block_registry_bindings(registry);
    return bindings ? (const maage_base_ids *) bindings->generation_context : NULL;
}
const maage_base_ids *maage_base_state_references(const maage_block_registry *registry, const maage_block_state_definition *state) {
    (void) state;
    return maage_base_generation_references(registry);
}

maage_status maage_base_bind(maage_content_context *context, maage_base_ids *ids) {
    maage_registry_bindings bindings;
    memset(&bindings, 0, sizeof(bindings));
    bindings.block_air = ids->block_air;
    bindings.state_air = ids->state_air;
    bindings.state_boundary = ids->state_bedrock;
    bindings.state_ui_background = ids->state_dirt;
    bindings.generation_context = ids;
    return maage_block_registry_bind(context->blocks, &bindings);
}
maage_status base_item_register(maage_content_context *context, const char *name, maage_item_definition *item, unsigned long fuel_ticks, maage_item_id *id) {
    maage_fuel_definition fuel;
    maage_status result;
    if (item->variant_count > 1) {
        if (item->has_block) item->icon_state = maage_base_item_icon;
        item->icon_texture = maage_base_item_texture;
        item->variant_name = maage_base_item_name;
    }
    item->texture_id = ((const maage_base_ids *) item->behavior_context)->textures[item->texture_id];
    result = maage_item_registry_register(context->items, name, item);
    if (maage_status_failed(result)) return result;
    *id = item->id;
    result = maage_base_item_variants(context, item, name);
    if (maage_status_failed(result)) return result;
    if (fuel_ticks) {
        memset(&fuel, 0, sizeof(fuel));
        fuel.item = item->id;
        fuel.variant = MAAGE_SMELTING_VARIANT_ANY;
        fuel.ticks = fuel_ticks;
        if (item->container_item) {
            fuel.remainder.item_id = item->container_item;
            fuel.remainder.count = 1;
        }
        result = maage_fuel_register(context->fuels, &fuel);
    }
    return result;
}
maage_status base_block_item(maage_content_context *context, const char *name, const char *label, maage_block_id block, maage_block_state_id state, unsigned int variant_limit, maage_item_placement placement, unsigned long fuel, maage_item_id *id) {
    maage_item_definition item;
    const maage_block_definition *type = maage_block_registry_block(context->blocks, block);
    const maage_block_state_definition *icon = maage_block_registry_state(context->blocks, state);
    if (!type || !icon) return maage_status_make(MAAGE_STATUS_NOT_FOUND, MAAGE_CONTEXT_REGISTRY, MAAGE_OPERATION_REGISTER, 0);
    memset(&item, 0, sizeof(item));
    item.behavior_context = context->module_data;
    item.has_block = 1;
    item.block_id = block;
    item.placement_state_id = item.icon_state_id = state;
    item.max_stack = 64;
    item.mining_speed = 1;
    item.texture_id = maage_base_logical_texture((const maage_base_ids *) item.behavior_context, icon->faces[MAAGE_FACE_POSITIVE_Z].texture_id);
    item.model = type->mesh_shape == MAAGE_BLOCK_MESH_CUBE ? MAAGE_ITEM_MODEL_BLOCK : MAAGE_ITEM_MODEL_SPRITE;
    item.variant_count = (size_t) variant_limit + 1;
    item.placement = placement;
    item.display_name = label;
    return base_item_register(context, name, &item, fuel, id);
}
maage_status base_tool(maage_content_context *context, const char *name, const char *label, unsigned int durability, unsigned int tool_class, unsigned int level, double speed, unsigned int texture, unsigned long fuel, maage_item_id *id) {
    maage_item_definition item;
    memset(&item, 0, sizeof(item));
    item.behavior_context = context->module_data;
    item.max_stack = 1;
    item.durability = durability;
    item.tool_tag = ((const maage_base_ids *) context->module_data)->tool_tags[tool_class];
    item.has_tool_tag = 1;
    item.tool_level = level;
    item.mining_speed = speed;
    if (tool_class == MAAGE_TOOL_PICKAXE) item.attack_damage = 3 + level;
    else if (tool_class == MAAGE_TOOL_AXE) item.attack_damage = 4 + level;
    else if (tool_class == MAAGE_TOOL_SHOVEL) item.attack_damage = 2 + level;
    item.model = MAAGE_ITEM_MODEL_SPRITE;
    item.texture_index = texture;
    item.texture_id = MAAGE_BASE_TEXTURE_ITEMS + texture;
    item.display_name = label;
    item.break_wear = tool_class == MAAGE_TOOL_HOE ? 0 : 1;
    item.use_durability = tool_class == MAAGE_TOOL_HOE;
    if (tool_class == MAAGE_TOOL_HOE) item.use_placement = maage_base_hoe_placement;
    return base_item_register(context, name, &item, fuel, id);
}
maage_status base_smelt(maage_smelting_registry *registry, maage_item_id input, maage_item_variant_id variant, maage_item_id output, maage_item_variant_id output_variant, double xp) {
    maage_smelting_recipe recipe;
    memset(&recipe, 0, sizeof(recipe));
    recipe.input = input;
    recipe.variant = variant;
    recipe.output.item_id = output;
    recipe.output.variant = output_variant;
    recipe.output.count = 1;
    recipe.experience = xp;
    return maage_smelting_register(registry, &recipe);
}
static maage_status maage_base_define(maage_content_context *context) {
    maage_base_ids *ids;
    maage_status result;
    ids = (maage_base_ids *) malloc(sizeof(*ids));
    if (!ids) return maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_REGISTRY, MAAGE_OPERATION_REGISTER, 0);
    memset(ids, 255, sizeof(*ids));
    result = maage_content_own(context, ids, free);
    if (maage_status_failed(result)) {
        free(ids);
        return result;
    }
    context->module_data = ids;
    /* Definitions precede every module's internal binding stage. */
    result = maage_base_traits(context, ids);
    if (!maage_status_failed(result)) result = maage_base_textures(context, ids);
    if (!maage_status_failed(result)) result = maage_base_sounds(context, ids);
    if (!maage_status_failed(result)) result = maage_base_bind(context, ids);
    if (!maage_status_failed(result)) result = maage_base_biomes(context->biomes, ids);
    if (!maage_status_failed(result)) result = maage_base_geometry(context, ids);
    if (!maage_status_failed(result)) result = maage_base_fluids_register(context, ids);
    if (!maage_status_failed(result)) result = maage_base_containers(context, ids);
    if (!maage_status_failed(result)) result = maage_base_blocks(context, ids);
    if (!maage_status_failed(result)) result = maage_base_items(context, ids);
    if (!maage_status_failed(result)) result = maage_base_flora(context, ids);
    if (!maage_status_failed(result)) result = maage_base_chest_models(context, ids);
    if (!maage_status_failed(result)) result = maage_base_entity_models(context, ids);
    if (!maage_status_failed(result)) result = maage_base_entities(context, ids);
    if (!maage_status_failed(result)) result = maage_base_dimensions(context, ids);
    if (!maage_status_failed(result)) result = maage_base_creative(context, ids);
    if (!maage_status_failed(result)) result = maage_base_dimension_creative(context, ids);
    if (!maage_status_failed(result)) result = maage_base_flora_creative(context, ids);
    if (!maage_status_failed(result)) result = maage_base_entity_creative(context, ids);
    if (!maage_status_failed(result)) result = maage_presentation_credit_append(context->presentation, "minecraft:end_poem", NULL, "minecraft/texts/end.txt");
    if (!maage_status_failed(result)) result = maage_presentation_credit_append(context->presentation, "minecraft:original_credits", NULL, "minecraft/texts/credits.txt");
    if (!maage_status_failed(result)) result = maage_presentation_credit_append(context->presentation, "minecraft:maage_credits", "[C]MaaGE\n\n[C]Created by FireController1847\n\n[C]Minecraft as a Game Engine\n[C]An independent native voxel platform\n\n[C]Thank you for playing.\n", NULL);
    return result;
}
static maage_status maage_base_bind_internal(maage_content_context *context) {
    maage_base_ids *ids = (maage_base_ids *) context->module_data;
    maage_biome_definition biome;
    size_t i;
    maage_status result = maage_base_bind(context, ids);
    if (!maage_status_failed(result)) result = maage_base_resolve_blocks(context, ids);
    if (!maage_status_failed(result)) result = maage_base_chest_models_bind(context, ids);
    if (!maage_status_failed(result)) result = maage_base_fluids_resolve(context, ids);
    if (!maage_status_failed(result)) result = maage_base_furnace_entity(context, ids);
    if (!maage_status_failed(result)) result = maage_base_chests_bind(context, ids);
    if (!maage_status_failed(result)) result = maage_base_traits_bind(context, ids);
    if (maage_status_failed(result)) return result;
    for (i = 0; i < MAAGE_BASE_BIOME_COUNT; ++i) {
        biome = *maage_biome_registry_get(context->biomes, ids->biome_ids[i]);
        biome.surface = ids->biome_grass[i];
        biome.filler = ids->state_dirt;
        if (biome.id == ids->biome_river || biome.id == ids->biome_frozen_river) biome.surface = ids->state_dirt;
        if (biome.id == ids->biome_stone_beach) biome.surface = biome.filler = ids->state_stone;
        if (biome.id == ids->biome_desert || biome.id == ids->biome_beach || biome.id == ids->biome_cold_beach) biome.surface = biome.filler = ids->state_sand;
        result = maage_biome_registry_update(context->biomes, &biome);
        if (maage_status_failed(result)) return result;
    }
    result = maage_base_generation_register(context, ids);
    if (!maage_status_failed(result)) result = maage_base_recipes(context, ids);
    if (!maage_status_failed(result)) result = maage_base_smelting(context, ids);
    return result;
}
const maage_content_module maage_base_module = {
    "minecraft", NULL, 0,
    maage_base_define, maage_base_bind_internal, NULL, NULL,
    0, 0, NULL, NULL, NULL, NULL};
