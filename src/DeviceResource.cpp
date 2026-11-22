#include "DeviceResource.h"

#include "TextureImage2D.h"
#include "TextureSamplerCombine.h"

void ImageViewCollector::visit(TextureImage2D &texture)
{
    vk::ImageView view = texture.imageView();
    if (view != vk::ImageView{}) {
        m_views.push_back(view);
    }
}

void ImageViewCollector::visit(TextureSamplerCombine &combine)
{
    vk::ImageView view = combine.imageView();
    if (view != vk::ImageView{}) {
        m_views.push_back(view);
    }
}
