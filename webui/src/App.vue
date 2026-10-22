<script setup lang="ts">
import { watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import AppShell from './components/AppShell.vue'
import OverlayHosts from './components/OverlayHosts.vue'
import { isSignedIn } from './stores/session'

const route = useRoute()
const router = useRouter()

// Expired or revoked keys end the session from anywhere in the app.
watch(isSignedIn, (signedIn) => {
  if (!signedIn && !route.meta.public) router.replace({ name: 'login', query: { redirect: route.fullPath } })
})
</script>

<template>
  <RouterView v-if="route.meta.public" />
  <AppShell v-else-if="isSignedIn" />
  <OverlayHosts />
</template>
