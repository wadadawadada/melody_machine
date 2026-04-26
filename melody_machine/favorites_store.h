#pragma once
#include <Arduino.h>
#include <vector>

struct FavoriteStation {
    String name;
    String url;
};

void favoritesInit();
int  favoritesCount();
const FavoriteStation& favoritesGet(int index);
// Returns true if added, false if already in favorites
bool favoritesAdd(const String& name, const String& url);
void favoritesRemove(int index);
bool favoritesSave();
bool favoritesContains(const String& url);
