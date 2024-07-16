/*
 * MPRIS 2 Server for Audacious
 * Copyright 2011-2012 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <math.h>
#include <stdint.h>
#include <vector>

#include <libaudcore/drct.h>
#include <libaudcore/hook.h>
#include <libaudcore/i18n.h>
#include <libaudcore/interface.h>
#include <libaudcore/plugin.h>
#include <libaudcore/probe.h>
#include <libaudcore/runtime.h>

#include "object-core.h"
#include "object-player.h"

class MPRIS2Plugin : public GeneralPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("MPRIS 2 Server"),
        PACKAGE
    };

    constexpr MPRIS2Plugin () : GeneralPlugin (info, true) {}

    bool init ();
    void cleanup ();
};

EXPORT MPRIS2Plugin aud_plugin_instance;
static GObject * object_core, * object_player;

static gboolean quit_cb (MprisMediaPlayer2 * object, GDBusMethodInvocation * call,
 void * unused)
{
    aud_quit ();
    mpris_media_player2_complete_quit (object, call);
    return true;
}

static gboolean raise_cb (MprisMediaPlayer2 * object, GDBusMethodInvocation *
 call, void * unused)
{
    aud_ui_show (true);
    mpris_media_player2_complete_raise (object, call);
    return true;
}

/**
 * @brief Struct for MPRIS2 metadata.
 */
struct MPRIS2Metadata
{
    String title;
    String artist;
    String album;
    String album_artist;
    String comment;
    String genre;
    String composer;
    String file;
    int32_t track = 0;
    int64_t length = 0;
    int32_t disc = 0;
    AudArtPtr image;

    MPRIS2Metadata() = default;

    MPRIS2Metadata(MPRIS2Metadata && other) noexcept
        : title(std::move(other.title)), artist(std::move(other.artist)),
          album(std::move(other.album)),
          album_artist(std::move(other.album_artist)),
          comment(std::move(other.comment)), genre(std::move(other.genre)),
          composer(std::move(other.composer)), file(std::move(other.file)),
          track(other.track), length(other.length), disc(other.disc),
          image(std::move(other.image))
    {
    }

    MPRIS2Metadata & operator=(MPRIS2Metadata && other) noexcept
    {
        if (this != &other)
        {
            title = std::move(other.title);
            artist = std::move(other.artist);
            album = std::move(other.album);
            album_artist = std::move(other.album_artist);
            comment = std::move(other.comment);
            genre = std::move(other.genre);
            composer = std::move(other.composer);
            file = std::move(other.file);
            track = other.track;
            length = other.length;
            disc = other.disc;
            image = std::move(other.image);
        }
        return *this;
    }

    bool operator==(const MPRIS2Metadata & other) const
    {
        return title == other.title && artist == other.artist &&
               album == other.album && album_artist == other.album_artist &&
               comment == other.comment && genre == other.genre &&
               composer == other.composer && file == other.file &&
               track == other.track && length == other.length &&
               disc == other.disc;
    }

    bool operator!=(const MPRIS2Metadata & other) const
    {
        return !(*this == other);
    }

    MPRIS2Metadata(const MPRIS2Metadata &) = delete;
    MPRIS2Metadata & operator=(const MPRIS2Metadata &) = delete;

    ~MPRIS2Metadata() { image.clear(); }
};

static MPRIS2Metadata last_meta;

/* Helper functions to handle GVariant creation */

void add_g_variant_str(const char * key_str, const char * value_str,
                       std::vector<GVariant *> & elems)
{
    if (value_str)
    {
        GVariant * key = g_variant_new_string(key_str);
        GVariant * str = g_variant_new_string(value_str);
        GVariant * var = g_variant_new_variant(str);
        elems.push_back(g_variant_new_dict_entry(key, var));
    }
}

void add_g_variant_int32(const char * key_str, int32_t value_int,
                         std::vector<GVariant *> & elems)
{
    GVariant * key = g_variant_new_string(key_str);
    GVariant * num = g_variant_new_int32(value_int);
    GVariant * var = g_variant_new_variant(num);
    elems.push_back(g_variant_new_dict_entry(key, var));
}

void add_g_variant_int64(const char * key_str, int64_t value_int,
                         std::vector<GVariant *> & elems)
{
    GVariant * key = g_variant_new_string(key_str);
    GVariant * num = g_variant_new_int64(value_int);
    GVariant * var = g_variant_new_variant(num);
    elems.push_back(g_variant_new_dict_entry(key, var));
}

void add_g_variant_arr(const char * key_str,
                       const std::vector<const char *> & value_arr,
                       std::vector<GVariant *> & elems)
{
    if (!value_arr.empty())
    {
        GVariant * key = g_variant_new_string(key_str);
        std::vector<GVariant *> g_variant_array;
        for (const auto & item : value_arr)
        {
            g_variant_array.push_back(g_variant_new_string(item));
        }
        GVariant * array =
            g_variant_new_array(G_VARIANT_TYPE_STRING, g_variant_array.data(),
                                g_variant_array.size());
        GVariant * var = g_variant_new_variant(array);
        elems.push_back(g_variant_new_dict_entry(key, var));
    }
}

static void update_metadata(void * data, GObject * object)
{
    MPRIS2Metadata meta;

    if (aud_drct_get_ready())
    {
        Tuple tuple = aud_drct_get_tuple();

        meta.title = tuple.get_str(Tuple::Title);
        meta.artist = tuple.get_str(Tuple::Artist);
        meta.album = tuple.get_str(Tuple::Album);
        meta.album_artist = tuple.get_str(Tuple::AlbumArtist);
        meta.comment = tuple.get_str(Tuple::Comment);
        meta.genre = tuple.get_str(Tuple::Genre);
        meta.composer = tuple.get_str(Tuple::Composer);
        meta.track = tuple.get_int(Tuple::Track);
        meta.length = tuple.get_int(Tuple::Length);
        meta.disc = tuple.get_int(Tuple::Disc);
        meta.file = aud_drct_get_filename();
    }

    if (meta == last_meta)
        return;

    if (meta.file != last_meta.file)
        meta.image =
            meta.file ? aud_art_request(meta.file, AUD_ART_FILE) : AudArtPtr();

    std::vector<GVariant *> elems;

    add_g_variant_str("xesam:title", meta.title, elems);
    add_g_variant_arr("xesam:artist", {meta.artist}, elems);
    add_g_variant_str("xesam:album", meta.album, elems);
    add_g_variant_arr("xesam:albumArtist", {meta.album_artist}, elems);
    add_g_variant_arr("xesam:comment", {meta.comment}, elems);
    add_g_variant_arr("xesam:genre", {meta.genre}, elems);
    add_g_variant_arr("xesam:composer", {meta.composer}, elems);
    add_g_variant_str("xesam:url", meta.file, elems);
    add_g_variant_int32("xesam:trackNumber", meta.track, elems);
    add_g_variant_int64("mpris:length", meta.length * 1000, elems);
    add_g_variant_int32("xesam:discNumber", meta.disc, elems);

    auto image_file = meta.image.file();
    add_g_variant_str("mpris:artUrl", image_file, elems);

    GVariant * key = g_variant_new_string("mpris:trackid");
    GVariant * str =
        g_variant_new_object_path("/org/mpris/MediaPlayer2/CurrentTrack");
    GVariant * var = g_variant_new_variant(str);
    elems.push_back(g_variant_new_dict_entry(key, var));

    GVariant * array =
        g_variant_new_array(G_VARIANT_TYPE("{sv}"), elems.data(), elems.size());
    g_object_set(object, "metadata", array, nullptr);

    last_meta = std::move(meta);
}

static void volume_changed (GObject * object)
{
    double vol;
    g_object_get (object, "volume", & vol, nullptr);
    aud_drct_set_volume_main (round (vol * 100));
}

static void update (void * object)
{
    int64_t pos = 0;
    int vol = 0;

    if (aud_drct_get_playing () && aud_drct_get_ready ())
        pos = (int64_t) aud_drct_get_time () * 1000;

    vol = aud_drct_get_volume_main ();

    g_signal_handlers_block_by_func (object, (void *) volume_changed, nullptr);
    g_object_set ((GObject *) object, "position", pos, "volume", (double) vol / 100, nullptr);
    g_signal_handlers_unblock_by_func (object, (void *) volume_changed, nullptr);
}

static void update_playback_status (void * data, GObject * object)
{
    const char * status;

    if (aud_drct_get_playing ())
        status = aud_drct_get_paused () ? "Paused" : "Playing";
    else
        status = "Stopped";

    g_object_set (object, "playback-status", status, nullptr);
    update (object);
}

static void emit_seek (void * data, GObject * object)
{
    g_signal_emit_by_name (object, "seeked", (int64_t) aud_drct_get_time () * 1000);
}

static gboolean next_cb (MprisMediaPlayer2Player * object, GDBusMethodInvocation *
 call, void * unused)
{
    aud_drct_pl_next ();
    mpris_media_player2_player_complete_next (object, call);
    return true;
}

static gboolean pause_cb (MprisMediaPlayer2Player * object,
 GDBusMethodInvocation * call, void * unused)
{
    if (aud_drct_get_playing () && ! aud_drct_get_paused ())
        aud_drct_pause ();

    mpris_media_player2_player_complete_pause (object, call);
    return true;
}

static gboolean play_cb (MprisMediaPlayer2Player * object, GDBusMethodInvocation *
 call, void * unused)
{
    aud_drct_play ();
    mpris_media_player2_player_complete_play (object, call);
    return true;
}

static gboolean play_pause_cb (MprisMediaPlayer2Player * object,
 GDBusMethodInvocation * call, void * unused)
{
    aud_drct_play_pause ();
    mpris_media_player2_player_complete_play_pause (object, call);
    return true;
}

static gboolean previous_cb (MprisMediaPlayer2Player * object,
 GDBusMethodInvocation * call, void * unused)
{
    aud_drct_pl_prev ();
    mpris_media_player2_player_complete_previous (object, call);
    return true;
}

static gboolean seek_cb (MprisMediaPlayer2Player * object,
 GDBusMethodInvocation * call, int64_t offset, void * unused)
{
    aud_drct_seek (aud_drct_get_time () + offset / 1000);
    mpris_media_player2_player_complete_seek (object, call);
    return true;
}

static gboolean set_position_cb (MprisMediaPlayer2Player * object,
 GDBusMethodInvocation * call, const char * track, int64_t pos, void * unused)
{
    if (aud_drct_get_playing ())
        aud_drct_seek (pos / 1000);

    mpris_media_player2_player_complete_set_position (object, call);
    return true;
}

static gboolean stop_cb (MprisMediaPlayer2Player * object, GDBusMethodInvocation *
 call, void * unused)
{
    if (aud_drct_get_playing ())
        aud_drct_stop ();

    mpris_media_player2_player_complete_stop (object, call);
    return true;
}

void MPRIS2Plugin::cleanup ()
{
    hook_dissociate ("playback begin", (HookFunction) update_playback_status);
    hook_dissociate ("playback pause", (HookFunction) update_playback_status);
    hook_dissociate ("playback stop", (HookFunction) update_playback_status);
    hook_dissociate ("playback unpause", (HookFunction) update_playback_status);

    hook_dissociate ("playback ready", (HookFunction) update_metadata);
    hook_dissociate ("playback stop", (HookFunction) update_metadata);
    hook_dissociate ("tuple change", (HookFunction) update_metadata);

    hook_dissociate ("playback ready", (HookFunction) emit_seek);
    hook_dissociate ("playback seek", (HookFunction) emit_seek);

    timer_remove (TimerRate::Hz4, update, object_player);

    g_object_unref (object_core);
    g_object_unref (object_player);

    last_meta = MPRIS2Metadata();
}

bool MPRIS2Plugin::init ()
{
    g_type_init ();

    GError * error = nullptr;
    GDBusConnection * bus = g_bus_get_sync (G_BUS_TYPE_SESSION, nullptr, & error);

    if (! bus)
    {
        AUDERR ("%s\n", error->message);
        g_error_free (error);
        return false;
    }

    g_bus_own_name_on_connection (bus, "org.mpris.MediaPlayer2.audacious",
     (GBusNameOwnerFlags) 0, nullptr, nullptr, nullptr, nullptr);

    object_core = (GObject *) mpris_media_player2_skeleton_new ();

    // build the schemes array
    auto schemes = VFSFile::supported_uri_schemes ();
    auto mimes = aud_plugin_get_supported_mime_types ();

    g_object_set (object_core,
     "can-quit", (gboolean) true,
     "can-raise", (gboolean) true,
     "desktop-entry", "audacious",
     "identity", "Audacious",
     "supported-uri-schemes", schemes.begin (),
     "supported-mime-types", mimes.begin (),
     nullptr);

    g_signal_connect (object_core, "handle-quit", (GCallback) quit_cb, nullptr);
    g_signal_connect (object_core, "handle-raise", (GCallback) raise_cb, nullptr);

    object_player = (GObject *) mpris_media_player2_player_skeleton_new ();

    g_object_set (object_player,
     "can-control", (gboolean) true,
     "can-go-next", (gboolean) true,
     "can-go-previous", (gboolean) true,
     "can-pause", (gboolean) true,
     "can-play", (gboolean) true,
     "can-seek", (gboolean) true,
     nullptr);

    update_playback_status (nullptr, object_player);

    if (aud_drct_get_playing () && aud_drct_get_ready ())
        emit_seek (nullptr, object_player);

    hook_associate ("playback begin", (HookFunction) update_playback_status, object_player);
    hook_associate ("playback pause", (HookFunction) update_playback_status, object_player);
    hook_associate ("playback stop", (HookFunction) update_playback_status, object_player);
    hook_associate ("playback unpause", (HookFunction) update_playback_status, object_player);

    hook_associate ("playback ready", (HookFunction) update_metadata, object_player);
    hook_associate ("playback stop", (HookFunction) update_metadata, object_player);
    hook_associate ("tuple change", (HookFunction) update_metadata, object_player);

    hook_associate ("playback ready", (HookFunction) emit_seek, object_player);
    hook_associate ("playback seek", (HookFunction) emit_seek, object_player);

    timer_add (TimerRate::Hz4, update, object_player);

    g_signal_connect (object_player, "handle-next", (GCallback) next_cb, nullptr);
    g_signal_connect (object_player, "handle-pause", (GCallback) pause_cb, nullptr);
    g_signal_connect (object_player, "handle-play", (GCallback) play_cb, nullptr);
    g_signal_connect (object_player, "handle-play-pause", (GCallback) play_pause_cb, nullptr);
    g_signal_connect (object_player, "handle-previous", (GCallback) previous_cb, nullptr);
    g_signal_connect (object_player, "handle-seek", (GCallback) seek_cb, nullptr);
    g_signal_connect (object_player, "handle-set-position", (GCallback) set_position_cb, nullptr);
    g_signal_connect (object_player, "handle-stop", (GCallback) stop_cb, nullptr);

    g_signal_connect (object_player, "notify::volume", (GCallback) volume_changed, nullptr);

    if (! g_dbus_interface_skeleton_export ((GDBusInterfaceSkeleton *)
     object_core, bus, "/org/mpris/MediaPlayer2", & error) ||
     ! g_dbus_interface_skeleton_export ((GDBusInterfaceSkeleton *)
     object_player, bus, "/org/mpris/MediaPlayer2", & error))
    {
        cleanup ();
        AUDERR ("%s\n", error->message);
        g_error_free (error);
        return false;
    }

    return true;
}
