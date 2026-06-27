/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"
#include <assets/assets.h>
#include <algorithm>
#include <cstdint>
#include <memory>

using namespace view;
using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace uitk::games;
using namespace uitk::games::dvd_screensaver;

static const Vector2 _screen_size               = {320, 240};
static const Vector2 _logo_size                 = {112, 100};
static const int _logo_id                       = 666;
static const uint32_t _bg_color = 0x000000;
static const uint32_t _ccflorb_width            = 1327;
static const uint32_t _ccflorb_height           = 1185;

static uint32_t fit_image_scale(uint32_t source_width, uint32_t source_height, uint32_t max_width, uint32_t max_height)
{
    if (source_width == 0 || source_height == 0 || max_width == 0 || max_height == 0) {
        return 256;
    }

    const uint32_t width_scale  = max_width * 256 / source_width;
    const uint32_t height_scale = max_height * 256 / source_height;
    return std::max<uint32_t>(1, std::min(width_scale, height_scale));
}

Screensaver::~Screensaver()
{
    _prev_screen->load();
}

void Screensaver::onInit()
{
    _prev_screen = std::make_unique<ScreenActive>();

    _screen = std::make_unique<Screen>();
    _screen->setBgColor(lv_color_hex(_bg_color));
    _screen->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _screen->setPadding(0, 0, 0, 0);
    _screen->load();

    _logo = std::make_unique<Container>(_screen->get());
    _logo->setSize(_logo_size.width, _logo_size.height);
    _logo->setBgOpa(0);
    _logo->align(LV_ALIGN_TOP_LEFT, 2333, 2333);
    _logo->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _logo->setPadding(0, 0, 0, 0);
    _logo->setBorderWidth(0);
    _logo->setRadius(0);

    _logo_image_dsc = assets::get_image("ccflorb.png");
    if (_logo_image_dsc.data_size != 0) {
        _logo_image = std::make_unique<Image>(_logo->get());
        _logo_image->setSrc(&_logo_image_dsc);
        _logo_image->setScale(
            fit_image_scale(_ccflorb_width, _ccflorb_height, _logo_size.width, _logo_size.height));
        _logo_image->align(LV_ALIGN_CENTER, 0, 0);
    }
}

void Screensaver::onBuildLevel()
{
    addScreenFrameAsWall(_screen_size);

    auto& random      = Random::getInstance();
    Vector2 direction = {random.getFloat(0.3, 0.7), random.getFloat(0.3, 0.7)};
    direction         = direction.normalized();

    addLogo(_logo_id, {_screen_size.width / 2, _screen_size.height / 2}, _logo_size, direction, 110);
}

void Screensaver::onRender(float dt)
{
    getWorld().forEachObject([&](GameObject* obj) {
        if (obj->groupId == _logo_id) {
            auto p = obj->get<Transform>()->position;
            _logo->setPos((int)p.x - _logo_size.width / 2, (int)p.y - _logo_size.height / 2);
        }
    });
}

void Screensaver::onLogoCollide(int logoGroupId)
{
    (void)logoGroupId;
}
