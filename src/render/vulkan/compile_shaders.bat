glslangValidator -D --sep main -e main -S frag --target-env vulkan1.0 --vn VULKAN_PixelShader_Colors -o VULKAN_PixelShader_Colors.h VULKAN_PixelShader_Colors.hlsl 
glslangValidator -D --sep main -e main -S frag --target-env vulkan1.0 --vn VULKAN_PixelShader_Textures -o VULKAN_PixelShader_Textures.h VULKAN_PixelShader_Textures.hlsl 
glslangValidator -D --sep main -e main -S frag --target-env vulkan1.0 --vn VULKAN_PixelShader_YUV -o VULKAN_PixelShader_YUV.h VULKAN_PixelShader_YUV.hlsl 
glslangValidator -D --sep main -e main -S frag --target-env vulkan1.0 --vn VULKAN_PixelShader_NV -o VULKAN_PixelShader_NV.h VULKAN_PixelShader_NV.hlsl 
glslangValidator -D --sep main -e main -S frag --target-env vulkan1.0 --vn VULKAN_PixelShader_HDR10 -o VULKAN_PixelShader_HDR10.h VULKAN_PixelShader_HDR10.hlsl 

glslangValidator -D --sep mainColor -e main -S vert --iy --target-env vulkan1.0 --vn VULKAN_VertexShader -o VULKAN_VertexShader.h VULKAN_VertexShader.hlsl 
