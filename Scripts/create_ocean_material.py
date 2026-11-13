"""Rebuild the synchronized Ocean material in Unreal's Python editor.

UnrealEditor-Cmd <project> -EnablePlugins=PythonScriptPlugin -run=pythonscript
  -script=<this file> -SCCProvider=None -unattended -NullRHI -NoAudio
"""

import unreal

PATH = "/LoomaSceneSync/Materials/M_LoomaOcean"
library = unreal.MaterialEditingLibrary
if unreal.EditorAssetLibrary.does_asset_exist(PATH):
    assert unreal.EditorAssetLibrary.delete_asset(PATH), PATH
material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    "M_LoomaOcean", "/LoomaSceneSync/Materials", unreal.Material, unreal.MaterialFactoryNew()
)
assert material
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
material.set_editor_property("tangent_space_normal", False)
material.set_editor_property("max_world_position_offset_displacement", 400.0)


def expression(kind):
    return library.create_material_expression(material, kind)


def vector(name, value):
    node = expression(unreal.MaterialExpressionVectorParameter)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("default_value", unreal.LinearColor(*value))
    # The first output is RGB. Combine the alpha for values packed by the actor.
    rgba = expression(unreal.MaterialExpressionAppendVector)
    assert library.connect_material_expressions(node, "", rgba, "A")
    assert library.connect_material_expressions(node, "A", rgba, "B")
    return rgba


def custom(description, inputs, code, output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3):
    node = expression(unreal.MaterialExpressionCustom)
    node.set_editor_property("description", description)
    node.set_editor_property("output_type", output_type)
    pins = []
    for name, _ in inputs:
        pin = unreal.CustomInput()
        pin.set_editor_property("input_name", name)
        pins.append(pin)
    node.set_editor_property("inputs", pins)
    node.set_editor_property("code", code)
    for name, source in inputs:
        assert library.connect_material_expressions(source, "", node, name), name
    return node


def connect(node, prop):
    assert library.connect_material_property(node, "", prop), prop


position = expression(unreal.MaterialExpressionWorldPosition)
position.set_editor_property("world_position_shader_offset", unreal.WorldPositionIncludedOffsets.WPT_EXCLUDE_ALL_SHADER_OFFSETS)
camera = expression(unreal.MaterialExpressionCameraVectorWS)
wind = vector("Wind", (-0.883, -0.469, 5.0, 0.4))
sea = vector("Sea", (8.0, 0.9, 0.0, 0.0))
floor = vector("Floor", (0.0, 0.0, 0.0, 0.0))


def texture(name):
    node = expression(unreal.MaterialExpressionTextureObjectParameter)
    node.set_editor_property("parameter_name", name)
    node.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black"))
    node.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    return node


heights = texture("FloorHeights")

# The grid samples the same heightfield as the terrain. World XY in centimetres
# becomes wire (X, Z) in metres, and the outer height samples are at texel centres.
depth = custom("Depth above the synced floor", [("World", position), ("Sea", sea), ("Floor", floor), ("Heights", heights)], """
    float2 delta = World.xy * 0.01 - Floor.xy;
    float2 uv = float2(delta.y, -delta.x) * Floor.z + 0.5;
    if (Floor.w < 2.0 || any(uv < 0.0) || any(uv > 1.0))
        return Sea.x;
    uv = (uv * (Floor.w - 1.0) + 0.5) / Floor.w;
    return -Texture2DSampleLevel(Heights, HeightsSampler, uv, 0).r;
""", unreal.CustomMaterialOutputType.CMOT_FLOAT1)

# Wave texels hold (Dx, height, Dz) in wire metres. Both paths use the
# same periodic samples, with different footprints for vertices and pixels.
spacing = expression(unreal.MaterialExpressionTextureCoordinate)
wave_inputs = [("World", position), ("Sea", sea), ("Depth", depth)]
wave_inputs += [(f"Wave{i}", texture(f"Wave{i}")) for i in range(4)]
lengths = (1103.0, 211.3, 39.7, 7.43)
wave_coords = """
    float2 xz = float2(World.y, -World.x) * 0.01;
    float centre = 0.5 / 128.0;
"""
geometry = wave_coords + """
    float3 disp = 0.0;
"""
lighting = wave_coords + """
    float footprint = max(length(ddx(xz)), length(ddy(xz)));
    float3 dx = 0.0;
    float3 dz = 0.0;
"""
for i, length in enumerate(lengths):
    geometry += f"""
    float lod{i} = log2(max(Spacing.x * (128.0 / {length}), 1.0)) + 0.5;
    disp += Texture2DSampleLevel(Wave{i}, Wave{i}Sampler, xz / {length} + centre, lod{i}).rgb;
"""
    lighting += f"""
    float lod{i} = min(7.0, log2(max(footprint * (128.0 / {length}), 1.0)));
    float step{i} = exp2(lod{i}) / 128.0;
    float2 uv{i} = xz / {length} + centre;
    float2 u{i} = float2(step{i}, 0.0);
    float2 v{i} = u{i}.yx;
    float scale{i} = 0.5 / (step{i} * {length});
    dx += (Texture2DSampleLevel(Wave{i}, Wave{i}Sampler, uv{i} + u{i}, lod{i}).rgb
         - Texture2DSampleLevel(Wave{i}, Wave{i}Sampler, uv{i} - u{i}, lod{i}).rgb) * scale{i};
    dz += (Texture2DSampleLevel(Wave{i}, Wave{i}Sampler, uv{i} + v{i}, lod{i}).rgb
         - Texture2DSampleLevel(Wave{i}, Wave{i}Sampler, uv{i} - v{i}, lod{i}).rgb) * scale{i};
"""
offset = custom("FFT displacement filtered to mesh spacing", wave_inputs + [("Spacing", spacing)], geometry + """
    disp *= smoothstep(0.0, Sea.y, Depth);
    return 100.0 * float3(-disp.z, disp.x, disp.y);
""")
normal = custom("Wave normals filtered to pixel footprint", wave_inputs, lighting + """
    float shoal = smoothstep(0.0, Sea.y, Depth);
    float3 tangentX = float3(1.0, 0.0, 0.0) + dx * shoal;
    float3 tangentZ = float3(0.0, 0.0, 1.0) + dz * shoal;
    float3 n = normalize(cross(tangentZ, tangentX));
    if (n.y < 0.0) n = -n;
    return float3(-n.z, n.x, n.y);
""")
color = custom("Shallow colour, surf and sky at grazing angles", [
    ("Camera", camera), ("Depth", depth), ("Wind", wind), ("Normal", normal), ("Offset", offset)
], """
    float3 shallow = float3(0.028, 0.20, 0.16);
    float3 deep = float3(0.008, 0.046, 0.066);
    float3 water = lerp(shallow, deep, smoothstep(0.0, 7.0, Depth));
    float grazing = pow(1.0 - saturate(dot(Camera, Normal)), 5.0);
    water = lerp(water, float3(0.09, 0.17, 0.24), 0.75 * grazing);
    float wash = saturate(0.5 + Offset.z * 0.04);
    float agitation = saturate(Wind.z * 0.1 + Wind.w);
    float surf = agitation * (1.0 - smoothstep(0.025, 0.10 + 0.16 * wash, Depth));
    return lerp(water, float3(0.28, 0.39, 0.38), surf);
""")
mask = custom("Exclude dry ground", [("Depth", depth)], "return step(0.001, Depth);",
              unreal.CustomMaterialOutputType.CMOT_FLOAT1)
roughness = expression(unreal.MaterialExpressionConstant)
roughness.set_editor_property("r", 0.16)
glow_amount = expression(unreal.MaterialExpressionConstant)
glow_amount.set_editor_property("r", 0.20)
glow = expression(unreal.MaterialExpressionMultiply)
assert library.connect_material_expressions(color, "", glow, "A")
assert library.connect_material_expressions(glow_amount, "", glow, "B")

connect(offset, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
connect(normal, unreal.MaterialProperty.MP_NORMAL)
connect(color, unreal.MaterialProperty.MP_BASE_COLOR)
connect(glow, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
connect(mask, unreal.MaterialProperty.MP_OPACITY_MASK)
connect(roughness, unreal.MaterialProperty.MP_ROUGHNESS)
library.layout_material_expressions(material)
library.recompile_material(material)
assert unreal.EditorAssetLibrary.save_asset(PATH), PATH
unreal.log("LOOMA_OCEAN_MATERIAL_SAVED " + PATH)
