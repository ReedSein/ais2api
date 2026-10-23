<script setup lang="ts">
import { watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppShell from './components/AppShell.vue'
import OverlayHosts from './components/OverlayHosts.vue'
import { isSignedIn, session } from './stores/session'

const route = useRoute()
const router = useRouter()

// Expired or revoked keys end the session from anywhere in the app.
watch(isSignedIn, (signedIn) => {
  if (!signedIn && !route.meta.public) router.replace({ name: 'login', query: { redirect: route.fullPath } })
})

// Any request refused with admin_password_change_required lands here.
watch(
  () => session.mustChangePassword,
  (required) => {
    if (required && isSignedIn.value && route.name !== 'change-password') router.replace({ name: 'change-password' })
  },
)
</script>

<template>
  <RouterView v-if="route.meta.public || (route.meta.bare && isSignedIn)" />
  <AppShell v-else-if="isSignedIn" />
  <OverlayHosts />
</template>
