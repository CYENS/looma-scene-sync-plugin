"""Rebuild the animated backdrop material in Unreal's Python editor.

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
material.set_editor_property("max_world_position_offset_displacement", 200.0)


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
time = expression(unreal.MaterialExpressionTime)
wind = vector("Wind", (-0.883, -0.469, 5.0, 0.4))
sea = vector("Sea", (8.0, 0.9, 0.0, 0.0))
floor = vector("Floor", (0.0, 0.0, 0.0, 0.0))
heights = expression(unreal.MaterialExpressionTextureObjectParameter)
heights.set_editor_property("parameter_name", "FloorHeights")
heights.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/Black"))
heights.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

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

# Three waves give displacement and analytic slopes from the same phases. Only
# the nearby grid is dense enough to displace. Pixel slopes retain longer waves
# past that range, and drop their fine detail before it becomes subpixel noise.
waves_code = """
    float2 p = World.xy * 0.01;
    float2 crossWind = float2(-Wind.y, Wind.x);
    float2 oblique = normalize(Wind.xy * 0.89 + crossWind * 0.46);
    float2 swellDir = normalize(Wind.xy * 0.92 - crossWind * 0.39);
    float3 k = float3(0.46, 1.09, 0.22);
    float3 phase = k * float3(dot(p, Wind.xy), dot(p, oblique), dot(p, swellDir))
        - Seconds * float3(2.12, 3.27, 1.47);
    float3 amplitude = float3(Wind.z * 0.028, Wind.z * 0.010, Wind.w * 0.5);
    amplitude *= smoothstep(0.0, Sea.y, Depth);
    float distanceToCamera = length(Camera.xy - World.xy) * 0.01;
"""
wave_inputs = [("World", position), ("Wind", wind), ("Sea", sea), ("Depth", depth), ("Seconds", time),
               ("Camera", expression(unreal.MaterialExpressionCameraPositionWS))]
offset = custom("Wave displacement", wave_inputs, waves_code + """
    float fade = 1.0 - smoothstep(25.0, 100.0, distanceToCamera);
    return float3(0.0, 0.0, 100.0 * fade * dot(amplitude, sin(phase)));
""")
normal = custom("World-space wave normal", wave_inputs, waves_code + """
    float3 slope = amplitude * k * cos(phase);
    slope *= 1.0 - smoothstep(float3(80.0, 40.0, 150.0), float3(360.0, 200.0, 700.0), distanceToCamera);
    float2 rippleDir = normalize(Wind.xy + 0.68 * crossWind);
    float ripple = 0.020 * Wind.z * cos(4.8 * dot(p, rippleDir) - 6.86 * Seconds);
    ripple *= (1.0 - smoothstep(25.0, 100.0, distanceToCamera)) * smoothstep(0.0, Sea.y, Depth);
    float2 slopeXY = slope.x * Wind.xy + slope.y * oblique + slope.z * swellDir + ripple * rippleDir;
    return normalize(float3(-slopeXY, 1.0));
""")
color = custom("Shallow colour, surf and sky at grazing angles", [
    ("World", position), ("Camera", camera), ("Depth", depth), ("Wind", wind), ("Normal", normal), ("Seconds", time)
], """
    float2 p = World.xy * 0.01;
    float3 shallow = float3(0.028, 0.20, 0.16);
    float3 deep = float3(0.008, 0.046, 0.066);
    float3 water = lerp(shallow, deep, smoothstep(0.0, 7.0, Depth));
    float grazing = pow(1.0 - saturate(dot(Camera, Normal)), 5.0);
    water = lerp(water, float3(0.09, 0.17, 0.24), 0.75 * grazing);
    float wash = 0.5 + 0.5 * sin(1.7 * dot(p, Wind.xy) - 1.6 * Seconds);
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
