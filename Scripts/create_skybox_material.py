"""Rebuild the cooked runtime panorama material in Unreal's Python editor.

UnrealEditor-Cmd <project> -EnablePlugins=PythonScriptPlugin -run=pythonscript
  -script=<this file> -unattended -NullRHI -NoAudio
The saved material is plugin content and must ship with the C++ implementation.
"""
import unreal

PATH = "/LoomaSceneSync/Materials/M_LoomaPanorama"
tools = unreal.AssetToolsHelpers.get_asset_tools()
material = unreal.load_asset(PATH)
if material is None:
    material = tools.create_asset("M_LoomaPanorama", "/LoomaSceneSync/Materials", unreal.Material, unreal.MaterialFactoryNew())
unreal.MaterialEditingLibrary.delete_all_material_expressions(material)
material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
material.set_editor_property("two_sided", True)
material.set_editor_property("is_sky", True)


def expression(kind):
    return unreal.MaterialEditingLibrary.create_material_expression(material, kind)


texture = expression(unreal.MaterialExpressionTextureObjectParameter)
texture.set_editor_property("parameter_name", "Panorama")
texture.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/DefaultTexture"))
camera = expression(unreal.MaterialExpressionCameraVectorWS)
yaw = expression(unreal.MaterialExpressionScalarParameter)
yaw.set_editor_property("parameter_name", "Yaw")
brightness = expression(unreal.MaterialExpressionScalarParameter)
brightness.set_editor_property("parameter_name", "Brightness")
brightness.set_editor_property("default_value", 1.0)
tint = expression(unreal.MaterialExpressionVectorParameter)
tint.set_editor_property("parameter_name", "Tint")
tint.set_editor_property("default_value", unreal.LinearColor(1, 1, 1, 1))
sample = expression(unreal.MaterialExpressionCustom)
sample.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
inputs = []
for name in ("Panorama", "Camera", "Yaw", "Brightness", "Tint"):
    item = unreal.CustomInput()
    item.set_editor_property("input_name", name)
    inputs.append(item)
sample.set_editor_property("inputs", inputs)
sample.set_editor_property("code", """
float3 ray = -Camera;
float3 d = normalize(float3(ray.y, ray.z, -ray.x));
d.xz = float2(cos(Yaw)*d.x-sin(Yaw)*d.z, sin(Yaw)*d.x+cos(Yaw)*d.z);
float2 uv = float2(atan2(d.z,d.x)/6.28318530718+0.5, 0.5-asin(clamp(d.y,-1,1))/3.14159265359);
return Texture2DSample(Panorama, PanoramaSampler, uv).rgb * Tint.rgb * Brightness;
""")
for source, name in ((texture, "Panorama"), (camera, "Camera"), (yaw, "Yaw"),
                     (brightness, "Brightness"), (tint, "Tint")):
    unreal.MaterialEditingLibrary.connect_material_expressions(source, "", sample, name)
unreal.MaterialEditingLibrary.connect_material_property(sample, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
# Camera-relative in each view, including stereo and scene captures.
position = expression(unreal.MaterialExpressionCameraPositionWS)
origin = expression(unreal.MaterialExpressionActorPositionWS)
offset = expression(unreal.MaterialExpressionSubtract)
unreal.MaterialEditingLibrary.connect_material_expressions(position, "", offset, "A")
unreal.MaterialEditingLibrary.connect_material_expressions(origin, "", offset, "B")
unreal.MaterialEditingLibrary.connect_material_property(offset, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
unreal.MaterialEditingLibrary.layout_material_expressions(material)
unreal.MaterialEditingLibrary.recompile_material(material)
unreal.EditorAssetLibrary.save_asset(PATH)
unreal.log("LOOMA_SKYBOX_MATERIAL_SAVED " + PATH)
